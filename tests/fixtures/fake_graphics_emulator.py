"""Test fixture implementing the emulator report contract."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


def provenance(test_id: str) -> dict[str, object]:
    return {
        "test_id": test_id,
        "build_id": "fixture-build",
        "gpu_name": "fixture-gpu",
        "gpu_vendor_id": 1,
        "gpu_device_id": 2,
        "gpu_driver": 3,
        "vulkan_api": 4,
        "configuration": "fixture-configuration",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game")
    parser.add_argument("--regression-compare")
    parser.add_argument("--regression-report", type=Path)
    parser.add_argument("--regression-frames")
    parser.add_argument("--regression-test-id")
    parser.add_argument("--regression-save-raw")
    parser.add_argument("--regression-allow-environment-mismatch")
    parser.add_argument("--fake-mode", default="pass")
    args, _ = parser.parse_known_args()
    if args.fake_mode == "spawn-child-sleep":
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        (Path.cwd() / "child.pid").write_text(str(child.pid), encoding="utf-8")
        time.sleep(30)
    if args.fake_mode == "sleep":
        time.sleep(30)
    if args.fake_mode == "no-report":
        return 0
    args.regression_report.parent.mkdir(parents=True, exist_ok=True)
    if args.fake_mode == "malformed":
        args.regression_report.write_text("null\n", encoding="utf-8")
        return 0
    frames = [int(value) for value in args.regression_frames.split(",")]
    matched = args.fake_mode not in ("mismatch", "bad-types")
    records = [{
        "ordinal": float(frame) if args.fake_mode == "bad-types" else frame,
        "width": 2,
        "height": 2,
        "row_pitch": 8,
        "byte_size": 16,
        "format": "rgba8_unorm",
        "hash_low": "0123456789abcdef",
        "hash_high": "fedcba9876543210",
        "expected_hash_low": "0123456789abcdef",
        "expected_hash_high": "fedcba9876543210",
        "status": "match" if matched else "mismatch",
    } for frame in frames]
    report = {
        "schema_version": True if args.fake_mode == "bad-types" else 2,
        "algorithm": "xxh3_128",
        "mode": "compare",
        "complete": True,
        "passed": matched,
        "baseline_provenance": provenance(args.regression_test_id),
        "run_provenance": provenance(args.regression_test_id),
        "environment_match": True,
        "frames": records,
    }
    args.regression_report.write_text(json.dumps(report), encoding="utf-8")
    if args.fake_mode == "artifact":
        artifacts = args.regression_report.parent / f"{args.regression_report.name}_frames"
        artifacts.mkdir(exist_ok=True)
        (artifacts / "current.txt").write_text("current", encoding="utf-8")
    return 7 if args.fake_mode == "nonzero-pass" else (0 if matched else 2)


if __name__ == "__main__":
    raise SystemExit(main())
