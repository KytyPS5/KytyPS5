#!/usr/bin/env python3
"""Isolated native auditor FP-state regressions; synthetic S_ENDPGM only."""
import argparse
import copy
import json
import math
from pathlib import Path
import struct
import tempfile
import subprocess
import sys


def run_case(executable, manifest, output, timeout):
    # Direct executable argv, no shell; no guest/GPU execution or child workers.
    flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
    with output.with_suffix(".stdout").open("wb") as stdout, \
         output.with_suffix(".stderr").open("wb") as stderr:
        process = subprocess.Popen([str(executable), "--audit-shader", str(manifest)],
                                   cwd=executable.parent, stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr, creationflags=flags)
        timed_out = False
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired as error:
                # Never continue with another native worker when exit is unconfirmed.
                raise RuntimeError("auditor termination unconfirmed; batch stopped") from error
        except BaseException:
            # Interruptions and unexpected wait failures must not leave a worker.
            process.kill()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError("auditor termination unconfirmed; batch stopped") from error
            raise
    return code, timed_out


MISSING_STATE = object()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output_directory", type=Path, nargs="?",
                        help="new evidence directory; preserved when explicitly supplied")
    parser.add_argument("--output-root", type=Path,
                        help="create a unique preserved evidence subdirectory under this root")
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or not executable.is_file():
        parser.error("positive timeout and a real native executable are required")
    if args.output_directory is not None and args.output_root is not None:
        parser.error("output_directory and --output-root are mutually exclusive")
    if args.output_directory is not None:
        output = args.output_directory.resolve()
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            parser.error("output directory must be absent or empty")
        output.mkdir(parents=True, exist_ok=True)
    else:
        output_root = None
        if args.output_root is not None:
            output_root = args.output_root.resolve()
            output_root.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="kyty-audit-metadata-", dir=output_root)).resolve()
    print(f"KYTY_AUDITOR_FP_STATE_EVIDENCE {output}", flush=True)
    (output / "end.bin").write_bytes(struct.pack("<I", 0xbf810000))
    base = {
        "schema_version": 1, "stage": "cs", "kind": "dispatched",
        "metadata_complete": True, "code_file": "end.bin",
        "wave_size": 32, "user_data_base": 0, "user_data_count": 0,
        "scratch_dwords": 0,
        "compute": {
            "threads_num": [32, 1, 1], "dispatch_threads_num": [1, 1, 1],
            "group_id": [False, False, False], "lds_size_dwords": 0,
            "scratch_size_dwords": 0, "dispatch_thread_dimensions": False,
            "needs_lds_barriers": False, "wave_size": 32,
            "thread_ids_num": 1, "workgroup_register": 0, "tg_size_en": False}}
    known = {"known": True, "float_mode": 192, "ieee_mode": False, "dx10_clamp": False}
    cases = [("legacy-dispatched", MISSING_STATE, False, {"known": False}, False),
             ("legacy-header-profile", MISSING_STATE, True, {"known": False}, False),
             ("known-default", known, False, known, False),
             ("known-header-profile", known, True, known, False),
             ("known-all-controls", dict(known, float_mode=255, ieee_mode=True,
                                          dx10_clamp=True), False,
              dict(known, float_mode=255, ieee_mode=True, dx10_clamp=True), False),
             ("known-zero", dict(known, float_mode=0), False,
              dict(known, float_mode=0), False),
             ("explicit-unknown", {"known": False}, False, {"known": False}, False)]
    for name, key, value in [("overflow", "float_mode", 256),
                             ("negative", "float_mode", -1),
                             ("fraction", "float_mode", 1.5),
                             ("string-mode", "float_mode", "192"),
                             ("boolean-mode", "float_mode", False),
                             ("integer-ieee", "ieee_mode", 1),
                             ("integer-clamp", "dx10_clamp", 1),
                             ("string-known", "known", "true")]:
        cases.append(("invalid-" + name, dict(known, **{key: value}), False, None, True))
    cases.append(("invalid-missing-fields", {"known": True}, False, None, True))
    for key in ("known", "float_mode", "ieee_mode", "dx10_clamp"):
        value = dict(known)
        del value[key]
        cases.append(("invalid-missing-" + key, value, False, None, True))
    # Unknown means no numeric promise. Valid but irrelevant payload is
    # canonicalized, while malformed supplied fields are never silently ignored.
    unknown = {"known": False, "float_mode": 0, "ieee_mode": False, "dx10_clamp": False}
    cases.append(("unknown-valid-payload", dict(known, known=False, float_mode=255,
                                               ieee_mode=True, dx10_clamp=True),
                  False, unknown, False))
    for name, state in [("null", None), ("list", []), ("string", "unknown"),
                        ("boolean", False), ("integer", 0)]:
        cases.append(("invalid-state-" + name, state, False, None, True))
    for name, key, value in [("string-mode", "float_mode", "192"),
                             ("overflow", "float_mode", 256),
                             ("negative", "float_mode", -1),
                             ("integer-ieee", "ieee_mode", 1),
                             ("null-clamp", "dx10_clamp", None)]:
        cases.append(("invalid-unknown-" + name, {"known": False, key: value},
                      False, None, True))
    cases.append(("invalid-state-empty", {}, False, None, True))
    report = []
    try:
        for name, state, profile, expected, invalid in cases:
            manifest = copy.deepcopy(base)
            if state is not MISSING_STATE:
                manifest["compute"]["initial_fp_state"] = state
            if profile:
                manifest.update(metadata_complete=False, metadata_provenance="agc_header_profile",
                                runtime_context_complete=False)
            path = output / (name + ".json")
            path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            code, timed_out = run_case(executable, path, output / name, args.timeout)
            lines = (output / name).with_suffix(".stdout").read_text(
                encoding="utf-8", errors="replace").splitlines()
            results = [json.loads(line[len("KYTY_SHADER_AUDIT_RESULT "): ])
                       for line in lines if line.startswith("KYTY_SHADER_AUDIT_RESULT ")]
            result = results[0] if len(results) == 1 else {}
            if invalid:
                passed = (not timed_out and code == 2 and
                          result.get("status") == "input_error" and result.get("phase") == "input")
            else:
                observed = result.get("initial_fp_state", {})
                passed = (not timed_out and code == 0 and result.get("status") == "passed" and
                          result.get("checked_through") == "resource_tracking" and
                          all(type(observed.get(k)) is type(v) and observed.get(k) == v
                              for k, v in expected.items()))
            report.append({"case": name, "passed": passed, "exit_code": code,
                           "timed_out": timed_out, "termination_confirmed": True,
                           "result": result})
            print(f"KYTY_AUDITOR_FP_STATE {'PASS' if passed else 'FAIL'} {name}", flush=True)
    except BaseException as error:
        report.append({"case": name, "passed": False, "runner_error": str(error)})
        raise
    finally:
        # Keep completed evidence even if process cleanup or result parsing fails.
        (output / "report.txt").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if all(item["passed"] for item in report) else 1


if __name__ == "__main__":
    raise SystemExit(main())
