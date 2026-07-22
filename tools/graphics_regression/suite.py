"""Validated sequential suite orchestration for one pinned GPU runner."""

from __future__ import annotations

import json
import re
import shutil
from dataclasses import asdict
from pathlib import Path

from .atomic import write_text as atomic_write_text
from .core import CaseSpec, run_case, validate_case

_CASE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")
_WINDOWS_DEVICE = re.compile(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", re.IGNORECASE)
_CASE_KEYS = frozenset({"id", "game", "baseline", "frames", "timeout_seconds", "save_raw",
                        "reuse_work_dir", "emulator_args", "enabled"})


def _overlaps(first: Path, second: Path) -> bool:
    return first == second or first in second.parents or second in first.parents


def load_suite(path: Path, emulator: Path, output: Path, launcher_args: tuple[str, ...],
               allow_environment_mismatch: bool) -> list[CaseSpec]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read suite: {error}") from error
    if (not isinstance(document, dict) or type(document.get("schema_version")) is not int or
            document["schema_version"] != 1 or not isinstance(document.get("cases"), list)):
        raise ValueError("suite must be a schema-version-1 object with a cases array")
    base = path.resolve().parent
    specs: list[CaseSpec] = []
    identifiers: set[str] = set()
    for entry in document["cases"]:
        if not isinstance(entry, dict) or set(entry) - _CASE_KEYS:
            raise ValueError("suite contains an invalid case object or unknown field")
        if "enabled" in entry and type(entry["enabled"]) is not bool:
            raise ValueError("suite case enabled field must be a boolean")
        if entry.get("enabled", True) is False:
            continue
        identifier = entry.get("id")
        portable_identifier = identifier.casefold() if isinstance(identifier, str) else ""
        if (not isinstance(identifier, str) or not _CASE_ID.fullmatch(identifier) or
                identifier.endswith(".") or _WINDOWS_DEVICE.fullmatch(identifier) or
                portable_identifier in identifiers):
            raise ValueError(f"invalid or duplicate case ID: {identifier!r}")
        identifiers.add(portable_identifier)
        if not isinstance(entry.get("game"), str) or not isinstance(entry.get("baseline"), str):
            raise ValueError(f"case {identifier} requires game and baseline paths")
        frames = entry.get("frames")
        if not isinstance(frames, list):
            raise ValueError(f"case {identifier} requires a frames array")
        arguments = entry.get("emulator_args", [])
        if not isinstance(arguments, list) or any(not isinstance(value, str) for value in arguments):
            raise ValueError(f"case {identifier} emulator_args must be strings")
        case_output = output.resolve() / identifier
        spec = CaseSpec(
            test_id=identifier,
            emulator=emulator.resolve(),
            game=(base / entry["game"]).resolve(),
            baseline=(base / entry["baseline"]).resolve(),
            report=case_output / "report.json",
            work_dir=case_output / "work",
            frames=frames,
            timeout_seconds=entry.get("timeout_seconds", 300.0),
            save_raw=entry.get("save_raw", False),
            reuse_work_dir=entry.get("reuse_work_dir", False),
            allow_environment_mismatch=allow_environment_mismatch,
            launcher_args=launcher_args,
            emulator_args=tuple(arguments),
        )
        validate_case(spec)
        specs.append(spec)
    if not specs:
        raise ValueError("suite has no enabled cases")
    output_root = output.resolve()
    inputs = [path.resolve(), emulator.resolve()]
    inputs += [value for spec in specs for value in (spec.game, spec.baseline)]
    if any(_overlaps(output_root, value) for value in inputs):
        raise ValueError("suite output directory overlaps a suite input")
    return specs


def run_suite(specs: list[CaseSpec], output: Path) -> int:
    output = output.resolve()
    if any(_overlaps(output, value) for spec in specs
           for value in (spec.emulator, spec.game, spec.baseline)):
        raise ValueError("suite output directory overlaps a case input")
    if any(spec.report.parent.parent != output or spec.work_dir.parent != spec.report.parent
           for spec in specs):
        raise ValueError("suite case outputs do not belong to the suite output directory")
    if output.is_symlink():
        raise ValueError(f"suite output directory cannot be a symlink: {output}")
    if output.exists() and not output.is_dir():
        raise ValueError(f"suite output path is not a directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    marker = output / ".runner-owned"
    if any(output.iterdir()) and (not marker.is_file() or marker.is_symlink()):
        raise ValueError(f"refusing to use unowned suite output directory: {output}")
    marker.touch(exist_ok=True)
    summary_path = output / "summary.json"
    current_case_directories = {spec.report.parent for spec in specs}
    for case_directory in current_case_directories:
        if case_directory.is_symlink() or (case_directory.exists() and not case_directory.is_dir()):
            raise ValueError(f"invalid suite case output directory: {case_directory}")
    for entry in output.iterdir():
        if entry == marker or entry in current_case_directories:
            continue
        if entry.is_symlink() or entry.is_file():
            entry.unlink()
        else:
            shutil.rmtree(entry)
    results = []
    for spec in specs:
        result = run_case(spec)
        results.append(asdict(result))
        atomic_write_text(
            summary_path,
            json.dumps({"schema_version": 1, "results": results}, indent=2) + "\n")
        print(f"graphics regression [{result.test_id}]: {result.message}")
    return 0 if all(result["exit_code"] == 0 for result in results) else 2
