#!/usr/bin/env python3
"""Run a validated graphics regression suite sequentially on one GPU."""

from __future__ import annotations

import argparse
from pathlib import Path

from graphics_regression.suite import load_suite, run_suite


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", required=True, type=Path)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--launcher-arg", action="append", default=[])
    parser.add_argument("--allow-environment-mismatch", action="store_true")
    args = parser.parse_args()
    try:
        specs = load_suite(args.suite, args.emulator, args.output, tuple(args.launcher_arg),
                           args.allow_environment_mismatch)
        return run_suite(specs, args.output.resolve())
    except (OSError, ValueError) as error:
        print(f"graphics regression suite: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
