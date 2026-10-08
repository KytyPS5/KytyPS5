#!/usr/bin/env python3
"""Extract an offline AGC shader corpus from a bounded KCAP container.

This recognizes the observed KCAP layout with a data-relative offset/size pair
at 0x48, AGC version 18 headers, and aligned header/code reference pairs. It
rejects other layouts and ambiguous references. Extracted metadata is not a
runtime resource snapshot and cannot establish complete shader compatibility.
The optional compute header profile supplies compiler inputs from declared
registers with explicit assumptions for missing dispatch and host GPU state.
Only the Python standard library is required. Game data belongs in ignored
local output directories, never in the source repository.
"""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


AGC_MAGIC = b"1234\x18\x00\x00\x00"
STAGES = ("cs", "ps", "gs", "hs", "gs_front", "hs_front", "gs_back", "hs_back", "fs")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unpack(data, fmt, offset):
    size = struct.calcsize(fmt)
    require(0 <= offset <= len(data) - size, f"out-of-bounds field at {offset:#x}")
    return struct.unpack_from(fmt, data, offset)


def u32(data, offset):
    return unpack(data, "<I", offset)[0]


def bitfields(value, fields):
    result = {}
    shift = 0
    for name, width in fields:
        result[name] = (value >> shift) & ((1 << width) - 1)
        shift += width
    return result


def parse_header(data, offset):
    require(data[offset:offset + 8] == AGC_MAGIC, "invalid AGC magic/version")
    require(offset + 96 <= len(data), "truncated AGC header")
    header_size, shader_size, embedded_size, target, num_inputs = unpack(data, "<5I", offset + 64)
    scratch, num_outputs, special_size, binary_type, num_cx, num_sh = unpack(data, "<3H3B", offset + 84)
    require(96 <= header_size <= len(data) - offset, f"invalid header size at {offset:#x}")
    require(shader_size > 0 and shader_size % 4 == 0, f"invalid code size at {offset:#x}")
    require(binary_type < len(STAGES), f"unsupported binary type {binary_type}")
    require(unpack(data, "<Q", offset + 16)[0] == 0, "expected an external code pointer")
    end = offset + header_size

    def field_pointer(field, count, element_size):
        raw = unpack(data, "<Q", field)[0]
        if count == 0:
            return None
        require(raw != 0, f"null metadata pointer at {field:#x}")
        target_offset = field + raw
        require(offset <= target_offset <= end - count * element_size,
                f"metadata array exceeds header at {field:#x}")
        return target_offset

    def registers(field, count):
        start = field_pointer(field, count, 8)
        return [{"offset": a, "value": b} for i in range(count)
                for a, b in [unpack(data, "<II", start + 8 * i)]]

    def semantics(field, count):
        start = field_pointer(field, count, 4)
        fields = (("semantic", 8), ("hardware_mapping", 8), ("size_in_elements", 4),
                  ("is_f16", 2), ("is_flat_shaded", 1), ("is_linear", 1),
                  ("is_custom", 1), ("static_vb_index", 1), ("static_attribute", 1),
                  ("reserved", 1), ("default_value", 2), ("default_value_hi", 2))
        return [bitfields(u32(data, start + 4 * i), fields) for i in range(count)]

    metadata = {
        "schema_version": 1, "kind": "extracted", "metadata_complete": False,
        "stage": STAGES[binary_type], "binary_type": binary_type,
        "file_header": u32(data, offset), "version": 18,
        "header_size": header_size, "shader_size_bytes": shader_size,
        "code_size_bytes": shader_size, "source_header_offset": offset,
        "embedded_constant_buffer_size_dqw": embedded_size, "target": target,
        "scratch_size_dwords": scratch, "special_sizes_bytes": special_size,
        "register_address_encoding": "shader_relative", "runtime_resources_captured": False,
        "num_cx_registers": num_cx, "num_sh_registers": num_sh,
        "num_input_semantics": num_inputs, "num_output_semantics": num_outputs,
        "cx_registers": registers(offset + 24, num_cx),
        "sh_registers": registers(offset + 32, num_sh),
        "input_semantics": semantics(offset + 48, num_inputs),
        "output_semantics": semantics(offset + 56, num_outputs),
        "user_data": None, "specials": None,
    }
    if unpack(data, "<Q", offset + 8)[0] != 0:
        user = field_pointer(offset + 8, 1, 56)
        eud, srt, direct_count, *sharp_counts = unpack(data, "<7H", user + 40)
        direct = field_pointer(user, direct_count, 2)
        sharp_arrays = []
        for index, count in enumerate(sharp_counts):
            sharp = field_pointer(user + 8 + index * 8, count, 2)
            sharp_arrays.append([{"offset_dw": value & 0x7fff, "size": value >> 15}
                                 for i in range(count)
                                 for value in [unpack(data, "<H", sharp + i * 2)[0]]])
        metadata["user_data"] = {
            "eud_size_dw": eud, "srt_size_dw": srt,
            "direct_resource_count": direct_count,
            "direct_resource_offsets": [unpack(data, "<H", direct + 2 * i)[0]
                                        for i in range(direct_count)],
            "sharp_resource_counts": sharp_counts, "sharp_resource_offsets": sharp_arrays,
        }
    if unpack(data, "<Q", offset + 40)[0] != 0:
        require(special_size == 48, f"unsupported special register size {special_size}")
        special = field_pointer(offset + 40, 1, 48)

        def register(at):
            a, b = unpack(data, "<II", special + at)
            return {"offset": a, "value": b}

        start, stop = unpack(data, "<HH", special + 20)
        draw = bitfields(u32(data, special + 24), (
            ("enbl_start_vertex_offset", 1), ("enbl_start_index_offset", 1),
            ("enbl_start_instance_offset", 1), ("enbl_draw_index", 1), ("enbl_user_vgprs", 1),
            ("render_target_slice_offset", 3), ("fuse_draws", 1), ("compiler_flags", 23)))
        draw.update(bitfields(u32(data, special + 28), (("is_default", 1), ("reserved", 31))))
        metadata["specials"] = {
            "ge_cntl": register(0), "vgt_shader_stages_en": register(8),
            "dispatch_modifier": u32(data, special + 16),
            "user_data_range": {"start": start, "end": stop}, "draw_modifier": draw,
            "vgt_gs_out_prim_type": register(32), "ge_user_vgpr_en": register(40),
        }
    return metadata


def add_compute_header_profile(metadata):
    """Model ShaderGetStaticInputInfoCS; retain missing runtime state as such."""
    if metadata["stage"] != "cs":
        return False
    registers = defaultdict(list)
    for register in metadata["sh_registers"]:
        registers[register["offset"]].append(register["value"])
    for offset in (0x207, 0x208, 0x209, 0x213):
        require(len(registers[offset]) == 1,
                f"compute header needs exactly one SH register {offset:#x}")
    require(metadata["specials"] is not None, "compute header has no dispatch modifier")
    rsrc2 = registers[0x213][0]
    require(len(registers[0x212]) <= 1, "ambiguous COMPUTE_PGM_RSRC1 FP state")
    initial_fp_state = {"known": False}
    if registers[0x212]:
        rsrc1 = registers[0x212][0]
        initial_fp_state = {
            "known": True, "float_mode": (rsrc1 >> 12) & 0xff,
            "ieee_mode": bool(rsrc1 & (1 << 23)),
            "dx10_clamp": bool(rsrc1 & (1 << 21)),
        }
    user_count = (rsrc2 >> 1) & 31
    # Pm4::ComputeWaveSize accepts a runtime modifier that can override this
    # header value. These are declared profiles, not captured dispatch inputs.
    wave_size = 32 if metadata["specials"]["dispatch_modifier"] & 0x8000 else 64
    scratch = metadata["scratch_size_dwords"]
    metadata.update({
        "metadata_provenance": "agc_header_profile",
        "metadata_complete": False, "runtime_context_complete": False,
        "wave_size": wave_size, "user_data_base": 0, "user_data_count": user_count,
        "scratch_dwords": scratch,
        "profile_assumptions": {
            "dispatch_modifier": "header value; runtime call site may override it",
            "dispatch_threads_num": "zero placeholders; dispatch dimensions were not captured",
            "dispatch_thread_dimensions": "assumed false; runtime dispatch mode was not captured",
            "needs_lds_barriers": "assumed true for wave64; audit both host capability variants",
        },
        "compute": {
            "initial_fp_state": initial_fp_state,
            "threads_num": [registers[offset][0] for offset in (0x207, 0x208, 0x209)],
            "dispatch_threads_num": [0, 0, 0],
            "lds_size_dwords": ((rsrc2 >> 15) & 511) * 128,
            "scratch_size_dwords": scratch,
            "group_id": [bool(rsrc2 & (1 << bit)) for bit in (7, 8, 9)],
            "dispatch_thread_dimensions": False,
            "needs_lds_barriers": wave_size == 64,
            "wave_size": wave_size, "thread_ids_num": ((rsrc2 >> 11) & 3) + 1,
            "workgroup_register": user_count, "tg_size_en": bool(rsrc2 & (1 << 10)),
        },
    })
    return True


def extract(data):
    require(len(data) >= 0x50 and data[:4] == b"KCAP", "expected a KCAP container")
    base, data_size = unpack(data, "<II", 0x48)
    require(0x50 <= base < len(data) and base + data_size == len(data),
            "unsupported KCAP data extent")
    offsets = [match.start() for match in re.finditer(re.escape(AGC_MAGIC), data)]
    require(offsets, "no AGC version 18 headers found")
    headers = {offset: parse_header(data, offset) for offset in offsets}
    header_end = base
    for offset, metadata in sorted(headers.items()):
        require(offset >= header_end and (offset - base) % 8 == 0,
                f"unaligned or overlapping header at {offset:#x}")
        header_end = offset + metadata["header_size"]
    code_start = base + ((header_end - base + 255) & ~255)
    relative_headers = {offset - base: offset for offset in headers}
    references = defaultdict(list)
    for at in range(base, code_start - 15, 8):
        relative_header, relative_code = unpack(data, "<QQ", at)
        if relative_header not in relative_headers or relative_code % 256 != 0:
            continue
        offset = relative_headers[relative_header]
        size = headers[offset]["shader_size_bytes"]
        code = base + relative_code
        if code_start <= code <= len(data) - size:
            references[offset].append((at, code, size))
    ranges = set()
    for offset, metadata in headers.items():
        require(len(references[offset]) == 1,
                f"expected one code reference at {offset:#x}, found {len(references[offset])}")
        reference, code, size = references[offset][0]
        metadata.update(source_code_offset=code, source_reference_offset=reference,
                        code_file=f"code_{code:08x}_{size:08x}.bin",
                        content_sha256=hashlib.sha256(data[code:code + size]).hexdigest())
        ranges.add((code, size))
    previous_end = code_start
    for code, size in sorted(ranges):
        require(code >= previous_end, f"overlapping distinct code ranges at {code:#x}")
        previous_end = code + size
    return headers, ranges, base, code_start


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, help="new, empty local corpus directory")
    parser.add_argument("--max-input-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--compute-header-profile", action="store_true",
                        help="add declared CS compiler inputs with explicit runtime assumptions")
    args = parser.parse_args()
    require(args.max_input_bytes > 0, "input limit must be positive")
    with args.input.open("rb") as source:
        data = source.read(args.max_input_bytes + 1)
    require(len(data) <= args.max_input_bytes, "input exceeds the configured byte limit")
    headers, ranges, base, code_start = extract(data)
    profile_count = 0
    if args.compute_header_profile:
        profile_count = sum(add_compute_header_profile(metadata) for metadata in headers.values())
    require(not args.output.exists() or (args.output.is_dir() and not any(args.output.iterdir())),
            "output directory must be absent or empty")
    # Validate the complete container before publishing any extracted payload.
    args.output.mkdir(parents=True, exist_ok=True)
    for code, size in sorted(ranges):
        (args.output / f"code_{code:08x}_{size:08x}.bin").write_bytes(data[code:code + size])
    for offset, metadata in sorted(headers.items()):
        path = args.output / f"{metadata['stage']}_{offset:08x}.json"
        path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    summary = {
        "input_bytes": len(data), "input_sha256": hashlib.sha256(data).hexdigest(),
        "headers": len(headers), "unique_code_ranges": len(ranges),
        "unique_code_bytes": sum(size for _, size in ranges),
        "stages": dict(sorted(Counter(value["stage"] for value in headers.values()).items())),
        "data_base": base, "code_region_start": code_start,
        "compute_header_profiles": profile_count,
        "metadata_complete": False, "output": str(args.output),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        print(f"shader corpus extraction failed: {error}", file=sys.stderr)
        sys.exit(1)
