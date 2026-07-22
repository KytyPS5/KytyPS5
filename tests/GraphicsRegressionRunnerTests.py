from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from graphics_regression import CaseSpec, run_case  # noqa: E402
from graphics_regression.report import validate_report  # noqa: E402
from graphics_regression.suite import load_suite, run_suite  # noqa: E402


class RunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="kyty_regression_runner_")
        self.root = Path(self.temporary.name)
        self.game = self.root / "game"
        self.game.mkdir()
        self.baseline = self.root / "baseline.json"
        self.write_baseline(self.baseline, "fixture-revision-checkpoint", [1, 3])
        self.fixture = ROOT / "tests/fixtures/fake_graphics_emulator.py"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def write_baseline(path: Path, test_id: str, frames: list[int]) -> None:
        provenance = {
            "test_id": test_id,
            "build_id": "fixture-build",
            "gpu_name": "fixture-gpu",
            "gpu_vendor_id": 1,
            "gpu_device_id": 2,
            "gpu_driver": 3,
            "vulkan_api": 4,
            "configuration": "fixture-configuration",
        }
        records = [{
            "ordinal": ordinal,
            "width": 2,
            "height": 2,
            "row_pitch": 8,
            "byte_size": 16,
            "format": "rgba8_unorm",
            "hash_low": "0123456789abcdef",
            "hash_high": "fedcba9876543210",
            "status": "recorded",
        } for ordinal in frames]
        path.write_text(json.dumps({
            "schema_version": 2,
            "algorithm": "xxh3_128",
            "mode": "record",
            "complete": True,
            "passed": True,
            "provenance": provenance,
            "frames": records,
        }), encoding="utf-8")

    def spec(self, mode: str = "pass", timeout: float = 5.0) -> CaseSpec:
        output = self.root / "output"
        return CaseSpec(
            test_id="fixture-revision-checkpoint",
            emulator=Path(sys.executable),
            launcher_args=(str(self.fixture),),
            game=self.game,
            baseline=self.baseline,
            report=output / "report.json",
            work_dir=output / "work",
            frames=[1, 3],
            timeout_seconds=timeout,
            emulator_args=("--fake-mode", mode),
        )

    def test_pass_and_log(self) -> None:
        result = run_case(self.spec())
        self.assertEqual(result.exit_code, 0)
        self.assertTrue(Path(result.log).is_file())

    def test_mismatch_and_process_report_disagreement(self) -> None:
        self.assertEqual(run_case(self.spec("mismatch")).exit_code, 2)
        self.assertEqual(run_case(self.spec("nonzero-pass")).exit_code, 7)

    def test_missing_and_non_object_reports_fail_cleanly(self) -> None:
        self.assertEqual(run_case(self.spec("no-report")).exit_code, 2)
        self.assertEqual(run_case(self.spec("malformed")).exit_code, 2)

    def test_timeout(self) -> None:
        result = run_case(self.spec("sleep", timeout=0.1))
        self.assertEqual(result.exit_code, 124)
        self.assertTrue(result.timed_out)

    @unittest.skipUnless(os.name == "nt", "Windows Job Object integration test")
    def test_windows_timeout_terminates_descendants(self) -> None:
        import ctypes
        from ctypes import wintypes

        spec = self.spec("spawn-child-sleep", timeout=2.0)
        result = run_case(spec)
        self.assertEqual(result.exit_code, 124)
        child_pid = int((spec.work_dir / "child.pid").read_text(encoding="utf-8"))
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
        kernel32.GetExitCodeProcess.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel32.OpenProcess(0x1000, False, child_pid)
        if handle:
            exit_code = wintypes.DWORD()
            self.assertTrue(kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)))
            kernel32.CloseHandle(handle)
            self.assertNotEqual(exit_code.value, 259)

    def test_stale_artifacts_are_removed(self) -> None:
        spec = self.spec("artifact")
        self.assertEqual(run_case(spec).exit_code, 0)
        stale = spec.report.parent / "report.json_frames/current.txt"
        self.assertTrue(stale.is_file())
        missing = self.spec("no-report")
        self.assertEqual(run_case(missing).exit_code, 2)
        self.assertFalse(stale.exists())

    def test_json_types_are_strict(self) -> None:
        spec = self.spec("bad-types")
        result = run_case(spec)
        self.assertEqual(result.exit_code, 2)
        valid, _ = validate_report(spec.report, spec.baseline, spec.frames, spec.test_id)
        self.assertFalse(valid)

    def test_matching_report_requires_equal_expected_hashes(self) -> None:
        spec = self.spec()
        self.assertEqual(run_case(spec).exit_code, 0)
        report = json.loads(spec.report.read_text(encoding="utf-8"))
        del report["frames"][0]["expected_hash_low"]
        spec.report.write_text(json.dumps(report), encoding="utf-8")
        self.assertFalse(validate_report(spec.report, spec.baseline, spec.frames, spec.test_id)[0])
        report["frames"][0]["expected_hash_low"] = "ffffffffffffffff"
        spec.report.write_text(json.dumps(report), encoding="utf-8")
        self.assertFalse(validate_report(spec.report, spec.baseline, spec.frames, spec.test_id)[0])

    def test_report_is_anchored_to_baseline_content(self) -> None:
        spec = self.spec()
        self.assertEqual(run_case(spec).exit_code, 0)
        report = json.loads(spec.report.read_text(encoding="utf-8"))
        report["frames"][0]["hash_low"] = "aaaaaaaaaaaaaaaa"
        report["frames"][0]["expected_hash_low"] = "aaaaaaaaaaaaaaaa"
        spec.report.write_text(json.dumps(report), encoding="utf-8")
        self.assertFalse(validate_report(spec.report, spec.baseline, spec.frames, spec.test_id)[0])
        self.assertEqual(run_case(spec).exit_code, 0)
        report = json.loads(spec.report.read_text(encoding="utf-8"))
        report["baseline_provenance"]["build_id"] = "forged-build"
        spec.report.write_text(json.dumps(report), encoding="utf-8")
        self.assertFalse(validate_report(spec.report, spec.baseline, spec.frames, spec.test_id)[0])

    def test_invalid_baseline_cannot_produce_a_pass(self) -> None:
        spec = self.spec()
        spec.baseline.write_text("{}", encoding="utf-8")
        self.assertEqual(run_case(spec).exit_code, 2)

    def test_result_write_does_not_use_fixed_temporary_path(self) -> None:
        spec = self.spec()
        fixed_temporary = spec.report.parent / "report.runner.json.tmp"
        fixed_temporary.parent.mkdir(parents=True)
        fixed_temporary.write_text("do not replace", encoding="utf-8")
        self.assertEqual(run_case(spec).exit_code, 0)
        self.assertEqual(fixed_temporary.read_text(encoding="utf-8"), "do not replace")

    def test_report_rejects_native_reservation_suffix(self) -> None:
        spec = replace(self.spec(), report=self.root / "result.kyty-lock")
        with self.assertRaisesRegex(ValueError, "reserved"):
            run_case(spec)

    def test_suite_rejects_duplicate_ids_before_running(self) -> None:
        suite = self.root / "suite.json"
        case = {"id": "duplicate", "game": "game", "baseline": "baseline.json", "frames": [1]}
        suite.write_text(json.dumps({"schema_version": 1, "cases": [case, case]}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            load_suite(suite, Path(sys.executable), self.root / "suite-output",
                       (str(self.fixture),), False)

    def test_suite_rejects_portable_path_aliases(self) -> None:
        suite = self.root / "suite.json"
        cases = [
            {"id": "Game", "game": "game", "baseline": "baseline.json", "frames": [1]},
            {"id": "game", "game": "game", "baseline": "baseline.json", "frames": [2]},
        ]
        suite.write_text(json.dumps({"schema_version": 1, "cases": cases}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            load_suite(suite, Path(sys.executable), self.root / "suite-output",
                       (str(self.fixture),), False)
        cases = [{"id": "NUL.txt", "game": "game", "baseline": "baseline.json", "frames": [1]}]
        suite.write_text(json.dumps({"schema_version": 1, "cases": cases}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "invalid"):
            load_suite(suite, Path(sys.executable), self.root / "suite-output",
                       (str(self.fixture),), False)

    def test_suite_runs_sequentially_and_writes_summary(self) -> None:
        suite = self.root / "suite.json"
        self.write_baseline(self.root / "passing.json", "passing-case", [1])
        self.write_baseline(self.root / "failing.json", "failing-case", [2])
        cases = [
            {"id": "passing-case", "game": "game", "baseline": "passing.json", "frames": [1]},
            {"id": "failing-case", "game": "game", "baseline": "failing.json", "frames": [2],
             "emulator_args": ["--fake-mode", "mismatch"]},
        ]
        suite.write_text(json.dumps({"schema_version": 1, "cases": cases}), encoding="utf-8")
        output = self.root / "suite-output"
        specs = load_suite(suite, Path(sys.executable), output, (str(self.fixture),), False)
        self.assertEqual(run_suite(specs, output), 2)
        summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual([result["exit_code"] for result in summary["results"]], [0, 2])

    def test_suite_removes_disabled_case_outputs(self) -> None:
        suite = self.root / "suite.json"
        self.write_baseline(self.root / "kept.json", "kept-case", [1])
        self.write_baseline(self.root / "removed.json", "removed-case", [2])
        cases = [
            {"id": "kept-case", "game": "game", "baseline": "kept.json", "frames": [1]},
            {"id": "removed-case", "game": "game", "baseline": "removed.json", "frames": [2]},
        ]
        suite.write_text(json.dumps({"schema_version": 1, "cases": cases}), encoding="utf-8")
        output = self.root / "suite-output"
        specs = load_suite(suite, Path(sys.executable), output, (str(self.fixture),), False)
        self.assertEqual(run_suite(specs, output), 0)
        stale = output / "removed-case" / "private-cache.bin"
        stale.write_bytes(b"stale")
        cases[1]["enabled"] = False
        suite.write_text(json.dumps({"schema_version": 1, "cases": cases}), encoding="utf-8")
        specs = load_suite(suite, Path(sys.executable), output, (str(self.fixture),), False)
        self.assertEqual(run_suite(specs, output), 0)
        self.assertFalse(stale.parent.exists())

    def test_suite_output_cannot_overwrite_a_baseline(self) -> None:
        output = self.root / "suite-output"
        output.mkdir()
        baseline = output / "summary.json"
        baseline.write_text("{}", encoding="utf-8")
        suite = self.root / "suite.json"
        suite.write_text(json.dumps({"schema_version": 1, "cases": [{
            "id": "collision", "game": "game", "baseline": str(baseline), "frames": [1]
        }]}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "overlaps"):
            load_suite(suite, Path(sys.executable), output, (str(self.fixture),), False)

    def test_case_output_cannot_overlap_another_case_input(self) -> None:
        output = self.root / "suite-output"
        cross_case_game = output / "first-case"
        cross_case_game.mkdir(parents=True)
        suite = self.root / "suite.json"
        cases = [
            {"id": "first-case", "game": "game", "baseline": "baseline.json", "frames": [1]},
            {"id": "second-case", "game": str(cross_case_game),
             "baseline": "baseline.json", "frames": [2]},
        ]
        suite.write_text(json.dumps({"schema_version": 1, "cases": cases}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "overlaps"):
            load_suite(suite, Path(sys.executable), output, (str(self.fixture),), False)


if __name__ == "__main__":
    unittest.main()
