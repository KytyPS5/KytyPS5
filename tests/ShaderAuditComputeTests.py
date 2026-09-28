#!/usr/bin/env python3
"""Synthetic native CPU audit phase contracts; never emit SPIR-V or launch GPU work."""
import argparse
import copy
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def run_worker(executable, manifest, host, output, timeout):
    args = [str(executable), "--audit-shader", str(manifest)]
    if host is not None:
        args += ["--compute-host-profile", str(host)]
    flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
    with output.with_suffix(".stdout").open("wb") as stdout, \
         output.with_suffix(".stderr").open("wb") as stderr:
        process = subprocess.Popen(args, cwd=executable.parent, stdin=subprocess.DEVNULL,
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
                raise RuntimeError("auditor termination unconfirmed; batch stopped") from error
        except BaseException:
            process.kill()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError("auditor termination unconfirmed; batch stopped") from error
            raise
    markers = [line[len("KYTY_SHADER_AUDIT_RESULT "):] for line in
               output.with_suffix(".stdout").read_text(encoding="utf-8", errors="replace").splitlines()
               if line.startswith("KYTY_SHADER_AUDIT_RESULT ")]
    result = json.loads(markers[0]) if len(markers) == 1 else {}
    return code, timed_out, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output_directory", type=Path, nargs="?")
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    if not executable.is_file() or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("real executable and positive finite timeout required")
    if args.output_directory is not None and args.output_root is not None:
        parser.error("output_directory and --output-root are mutually exclusive")
    if args.output_directory is not None:
        output = args.output_directory.resolve()
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            parser.error("output directory must be absent or empty")
        output.mkdir(parents=True, exist_ok=True)
    else:
        if args.output_root is not None:
            args.output_root.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="compute-precheck-", dir=args.output_root)).resolve()
    print(f"KYTY_AUDITOR_COMPUTE_EVIDENCE {output}", flush=True)
    host = {"schema_version": 1, "max_size": [1024, 1024, 64], "max_invocations": 1024,
            "native_subgroup_size": 32, "can_require_subgroup_size_64": False,
            "max_shared_memory_bytes": 49152}
    (output / "end.bin").write_bytes(struct.pack("<I", 0xbf810000))
    # V_MOV_B32 v1,EXEC_LO; BUFFER_STORE_DWORD v1,v0,s[0:3]; S_ENDPGM.
    # The raw mask is observable, so a wave operation survives translation/DCE.
    # No descriptor payload is materialized and the output is never executed.
    mask = [0x7e02027e, 0xe0701000, 0x80000100, 0xbf810000]
    (output / "mask.bin").write_bytes(struct.pack("<4I", *mask))
    # Constant value and address do not retain any native wave operation.
    # Thus an unknown ADD_TID is material to local33 admission: false passes,
    # true requires software wave64 and rejects the incomplete guest wave.
    plain = [0x7e020281, 0x7e000280, 0xe0701000, 0x80000100, 0xbf810000]
    (output / "plain.bin").write_bytes(struct.pack("<5I", *plain))
    base = {"schema_version": 1, "kind": "dispatched", "stage": "cs",
            "metadata_complete": True, "code_file": "end.bin", "wave_size": 32,
            "user_data_base": 0, "user_data_count": 0, "scratch_dwords": 0,
            "compute": {"threads_num": [32, 1, 1], "dispatch_threads_num": [1, 1, 1],
                        "group_id": [False, False, False], "lds_size_dwords": 0,
                        "scratch_size_dwords": 0, "dispatch_thread_dimensions": False,
                        "needs_lds_barriers": False, "wave_size": 32, "thread_ids_num": 1,
                        "workgroup_register": 0, "tg_size_en": False}}
    cases = []
    def add(name, manifest, profile, expected, profiles=1, attempts=1, error=""):
        cases.append((name, copy.deepcopy(manifest), copy.deepcopy(profile),
                      expected, profiles, attempts, error))
    add("legacy-default", base, None, "legacy")
    add("valid-native32", base, host, "not_rejected")
    oversized = copy.deepcopy(base)
    oversized["compute"]["threads_num"] = [1025, 1, 1]
    add("oversized-workgroup", oversized, host, "rejected", error="cannot fit device")
    header = copy.deepcopy(base)
    header.update(metadata_complete=False, metadata_provenance="agc_header_profile",
                  runtime_context_complete=False)
    add("header-both-barriers", header, host, "not_rejected", profiles=2)
    header["compute"]["threads_num"] = [1025, 1, 1]
    add("header-collect-both-errors", header, host, "rejected", profiles=2,
        error="cannot fit device")
    partial = copy.deepcopy(base)
    partial.update(code_file="mask.bin", user_data_count=4, wave_size=64)
    partial["compute"].update(threads_num=[33, 1, 1], wave_size=64)
    add("partial-wave64", partial, host, "rejected", attempts=2,
        error="complete guest waves")
    conditional = copy.deepcopy(partial)
    conditional["code_file"] = "plain.bin"
    add("conditional-add-tid", conditional, host, "conditional", attempts=2)
    partial["compute"]["threads_num"] = [64, 1, 1]
    add("complete-wave64-unknown-descriptor", partial, host, "not_rejected", attempts=2)
    missing = copy.deepcopy(base)
    missing["metadata_complete"] = False
    del missing["compute"]
    add("missing-compute-input", missing, host, "not_checked")
    missing["stage"] = "ps"
    add("pixel-cfg-only", missing, host, "not_checked")
    for name, key, value in [("zero-native", "native_subgroup_size", 0),
                             ("zero-limit", "max_invocations", 0),
                             ("overflow-limit", "max_invocations", 2**32),
                             ("boolean-limit", "max_invocations", True),
                             ("fraction-limit", "max_invocations", 1.5),
                             ("short-dimensions", "max_size", [1024, 64]),
                             ("negative-dimension", "max_size", [1024, -1, 64]),
                             ("string-capability", "can_require_subgroup_size_64", "false"),
                             ("wrong-schema", "schema_version", 2)]:
        bad_host = dict(host, **{key: value})
        add("invalid-host-" + name, base, bad_host, "input_error")
    bad_host = dict(host)
    del bad_host["max_shared_memory_bytes"]
    add("invalid-host-missing-field", base, bad_host, "input_error")
    report = []
    name = "setup"
    try:
        for name, manifest, profile, expected, profiles, attempts, error in cases:
            manifest_path = output / (name + ".json")
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            host_path = None
            if profile is not None:
                host_path = output / (name + "-host.json")
                host_path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
            code, timed_out, result = run_worker(executable, manifest_path, host_path,
                                                 output / name, args.timeout)
            passed = not timed_out
            if expected == "legacy":
                passed &= (code == 0 and result.get("status") == "passed" and
                           result.get("checked_through") == "resource_tracking" and
                           "compute_execution_precheck" not in result)
            elif expected == "input_error":
                passed &= (code == 2 and result.get("status") == "input_error" and
                           result.get("phase") == "input")
            else:
                summary = result.get("compute_execution_precheck", {})
                expected_status = "not_rejected" if expected == "conditional" else expected
                passed &= (summary.get("status") == expected_status and
                           summary.get("post_specialization_checked") is False and
                           summary.get("spirv_emitted") is False and
                           result.get("gpu_execution_checked") is False and
                           result.get("resource_materialization_checked") is False)
                if expected == "not_checked":
                    passed &= (code == 0 and result.get("checked_through") == "cfg" and
                               bool(summary.get("reason")))
                else:
                    rows = result.get("profiles_checked", [])
                    passed &= (code == (1 if expected == "rejected" else 0) and
                               result.get("status") == ("failed" if expected == "rejected" else "passed") and
                               result.get("checked_through") == "compute_execution_precheck" and
                               len(rows) == profiles)
                    if profiles == 2:
                        passed &= {row.get("needs_lds_barriers") for row in rows} == {False, True}
                    for row in rows:
                        check = row.get("compute_execution_precheck", {})
                        variants = check.get("requirements_variants", [])
                        passed &= (check.get("status") == expected_status and len(variants) == attempts and
                                   check.get("post_specialization_checked") is False and
                                   check.get("spirv_emitted") is False)
                        if attempts == 2:
                            passed &= bool(check.get("unknown_requirements"))
                            passed &= {v.get("assumed_buffer_add_tid") for v in variants} == {False, True}
                        if expected == "conditional":
                            passed &= (not check.get("errors") and
                                       any("complete guest waves" in text for text in check.get("possible_errors", [])) and
                                       [v.get("status") for v in variants] == ["not_rejected", "rejected"])
                        if error:
                            passed &= any(error in text for text in check.get("errors", []))
            report.append({"case": name, "passed": bool(passed), "exit_code": code,
                           "timed_out": timed_out, "termination_confirmed": True, "result": result})
            print(f"KYTY_AUDITOR_COMPUTE {'PASS' if passed else 'FAIL'} {name}", flush=True)
    except BaseException as error:
        report.append({"case": name, "passed": False, "runner_error": str(error)})
        raise
    finally:
        (output / "report.txt").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if all(row["passed"] for row in report) else 1


if __name__ == "__main__":
    raise SystemExit(main())
