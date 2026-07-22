#!/usr/bin/env python3
"""Run one deterministic graphics comparison with timeout and report validation."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


FAILURE = 2
TIMEOUT = 124


def frame_ordinals(value: str) -> list[int]:
    try:
        frames = [int(part) for part in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("frames must be comma-separated integers") from error
    if not frames or any(frame < 0 for frame in frames) or len(frames) != len(set(frames)):
        raise argparse.ArgumentTypeError("frames must be unique non-negative integers")
    return frames


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--frames", required=True, type=frame_ordinals)
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument("--no-save-raw", action="store_true")
    parser.add_argument("emulator_args", nargs=argparse.REMAINDER,
                        help="additional emulator arguments after --")
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    return args


def validate_report(path: Path, frames: list[int]) -> tuple[bool, str]:
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return False, f"cannot read a valid report: {error}"
    if (report.get("schema_version") != 1 or report.get("mode") != "compare" or
            report.get("algorithm") != "xxh3_128" or report.get("complete") is not True):
        return False, "report metadata is invalid or incomplete"
    records = report.get("frames")
    if (not isinstance(records, list) or any(not isinstance(record, dict) for record in records) or
            [record.get("ordinal") for record in records] != sorted(frames)):
        return False, "report does not contain exactly the requested frames"
    passed = report.get("passed") is True and all(record.get("status") == "match"
                                                    for record in records)
    return passed, "all selected frames match" if passed else "one or more frames differ"


def main() -> int:
    args = parse_args()
    try:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        # A timed-out run must not reuse a stale successful report.
        args.report.unlink(missing_ok=True)
    except OSError as error:
        print(f"cannot prepare report path: {error}", file=sys.stderr)
        return FAILURE
    frames = ",".join(str(frame) for frame in args.frames)
    command = [
        str(args.emulator), "--game", str(args.game),
        "--regression-compare", str(args.baseline),
        "--regression-report", str(args.report),
        "--regression-frames", frames,
        "--regression-save-raw", "false" if args.no_save_raw else "true",
    ]
    if args.emulator_args and args.emulator_args[0] == "--":
        args.emulator_args.pop(0)
    command.extend(args.emulator_args)
    try:
        result = subprocess.run(command, timeout=args.timeout_seconds, check=False)
    except subprocess.TimeoutExpired:
        print(f"graphics regression timed out after {args.timeout_seconds:g}s", file=sys.stderr)
        return TIMEOUT
    except OSError as error:
        print(f"could not start emulator: {error}", file=sys.stderr)
        return FAILURE

    passed, message = validate_report(args.report, args.frames)
    print(f"graphics regression: {message}")
    if not passed:
        return result.returncode if result.returncode != 0 else FAILURE
    if result.returncode != 0:
        print(f"emulator returned {result.returncode} despite a passing report", file=sys.stderr)
        return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
