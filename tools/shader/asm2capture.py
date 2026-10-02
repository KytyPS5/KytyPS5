#!/usr/bin/env python3
"""Assemble an RDNA2 compute shader and wrap it as a `--shader-replay` capture.

    asm2capture.py shader.s out_dir [--user-data 0x1,0x2,...] [--hash 0x...] [--threads 64,1,1]

Needs llvm-mc with the AMDGPU target (`llvm-mc -triple=amdgcn`). The capture has the layout that
the emulator writes with --shader-capture-dir (see src/graphics/shader/shaderCapture.h), so it can
be replayed with `kyty_emulator --shader-replay out_dir`. Intended for regression fixtures and for
reproducing a recompiler failure from a few lines of assembly.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

FORMAT = 1


def find_llvm_mc():
    for name in ("llvm-mc", "llvm-mc-18", "llvm-mc-17", "llvm-mc-19", "llvm-mc-20"):
        path = shutil.which(name)
        if path:
            return path
    sys.exit("llvm-mc not found; install LLVM with the AMDGPU target")


def elf_text_section(data: bytes) -> bytes:
    """Return the .text section of a little-endian ELF64 object."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("assembler did not produce a little-endian ELF64 object")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = []
    for index in range(shnum):
        base = shoff + index * shentsize
        name, kind, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", data, base)
        sections.append((name, offset, size))
    strings_offset = sections[shstrndx][1]
    for name, offset, size in sections:
        end = data.index(b"\0", strings_offset + name)
        if data[strings_offset + name:end] == b".text":
            return data[offset:offset + size]
    sys.exit("no .text section in assembled object")


def assemble(source: str, mcpu: str, mattr: str) -> bytes:
    # An object file, unlike --show-encoding, has label branches resolved.
    result = subprocess.run(
        [find_llvm_mc(), "-triple=amdgcn", f"-mcpu={mcpu}", f"-mattr={mattr}", "-filetype=obj"],
        input=source.encode(), capture_output=True)
    if result.returncode != 0:
        sys.exit(f"assembly failed:\n{result.stderr.decode()}")
    code = elf_text_section(result.stdout)
    if not code or len(code) % 4:
        sys.exit("assembler produced no code or a partial dword")
    return code


def xxh3_64(data: bytes) -> int:
    try:
        import xxhash  # type: ignore
        return xxhash.xxh3_64_intdigest(data)
    except ImportError:
        return 0


def parse_ints(text: str):
    return [int(part, 0) for part in text.split(",") if part.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("assembly", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--user-data", default="", help="comma separated user SGPR values")
    parser.add_argument("--hash", default=None, help="shader hash to record (default: XXH3 of the code, or a fixed value)")
    parser.add_argument("--threads", default="64,1,1", help="threads per group x,y,z")
    parser.add_argument("--wave-size", type=int, default=64)
    parser.add_argument("--mcpu", default="gfx1030")
    parser.add_argument("--mattr", default=None, help="llvm-mc -mattr (default: wavefrontsize64 or 32 to match --wave-size)")
    parser.add_argument("--reads", type=Path, default=None,
                        help="JSON list of {\"address\": \"0x..\", \"words\": [..]} guest reads to record")
    args = parser.parse_args()

    mattr = args.mattr or ("+wavefrontsize64" if args.wave_size == 64 else "+wavefrontsize32")
    code = assemble(args.assembly.read_text(), args.mcpu, mattr)
    user_data = parse_ints(args.user_data)
    threads = parse_ints(args.threads)
    if len(threads) != 3:
        sys.exit("--threads needs three values")
    shader_hash = int(args.hash, 0) if args.hash else (xxh3_64(code) or 0x1234567890abcdef)
    static_state = [len(user_data), args.wave_size | (0xC0 << 8), 64, 1, 0, 0, 0, threads[0], 1, threads[1], 0, threads[2], 0, 0]
    key = 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "code.bin").write_bytes(code)
    (args.out_dir / "user_data.bin").write_bytes(struct.pack(f"<{len(user_data)}I", *user_data))
    reads = bytearray()
    if args.reads:
        for read in json.loads(args.reads.read_text()):
            words = [int(w, 0) if isinstance(w, str) else w for w in read["words"]]
            reads += struct.pack("<QII", int(read["address"], 0), len(words), 1)
            reads += struct.pack(f"<{len(words)}I", *words)
    (args.out_dir / "reads.bin").write_bytes(bytes(reads))
    manifest = {
        "format": FORMAT,
        "stage": "cs",
        "hash": f"0x{shader_hash:016x}",
        "key": f"{key:08x}",
        "wave_size": args.wave_size,
        "user_data_base": 0,
        "user_data_count": len(user_data),
        "code_size_bytes": len(code),
        "back_code_size_bytes": 0,
        "shader_base": "0x0000000000000000",
        "static_state": static_state,
        "input": {
            "kind": "compute",
            # Workgroup ids follow the user SGPRs, so s0..s(n-1) hold the user data.
            "workgroup_register": len(user_data),
            "wave_size": args.wave_size,
            "float_mode": 0xC0,
            "host_subgroup_size": 64,
            "thread_ids_num": 1,
            "lds_size_dwords": 0,
            "scratch_size_dwords": 0,
            "dispatch_thread_dimensions": False,
            "tg_size_en": False,
            "threads_num": threads,
            "dispatch_threads_num": [0, 0, 0],
            "group_id": [True, False, False],
        },
        "replay": True,
        "git_revision": "asm2capture",
        "git_hash": "asm2capture",
        "build": "asm2capture",
    }
    (args.out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {args.out_dir} ({len(code)} bytes of code, hash 0x{shader_hash:016x})")


if __name__ == "__main__":
    main()
