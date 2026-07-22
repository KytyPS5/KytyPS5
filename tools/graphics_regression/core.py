"""Single-case graphics regression orchestration."""

from __future__ import annotations

import json
import math
import shutil
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from .atomic import write_text as atomic_write_text
from .process import TIMEOUT_EXIT_CODE, run_process
from .report import validate_report

FAILURE = 2
MAX_FRAMES = 4096
MAX_ORDINAL = (1 << 64) - 1
_OWNED_FLAGS = frozenset({
    "--game", "--regression-record", "--regression-compare", "--regression-report",
    "--regression-frames", "--regression-test-id", "--regression-save-raw",
    "--regression-exit-on-complete", "--regression-allow-environment-mismatch",
})


@dataclass(frozen=True)
class CaseSpec:
    test_id: str
    emulator: Path
    game: Path
    baseline: Path
    report: Path
    work_dir: Path
    frames: list[int]
    timeout_seconds: float = 300.0
    save_raw: bool = False
    reuse_work_dir: bool = False
    allow_environment_mismatch: bool = False
    launcher_args: tuple[str, ...] = ()
    emulator_args: tuple[str, ...] = ()


@dataclass(frozen=True)
class CaseResult:
    test_id: str
    exit_code: int
    message: str
    timed_out: bool
    duration_seconds: float
    report: str
    log: str


def _artifact_directory(report: Path) -> Path:
    return report.parent / f"{report.name}_frames"


def _contains_owned_flag(arguments: tuple[str, ...]) -> bool:
    return any(argument.split("=", 1)[0] in _OWNED_FLAGS for argument in arguments)


def validate_case(spec: CaseSpec) -> None:
    if not spec.test_id or len(spec.test_id) > 4096:
        raise ValueError("test ID must contain 1-4096 characters")
    if (not spec.frames or len(spec.frames) > MAX_FRAMES or len(spec.frames) != len(set(spec.frames)) or
            any(type(frame) is not int or frame < 0 or frame > MAX_ORDINAL for frame in spec.frames)):
        raise ValueError("frames must be unique unsigned 64-bit integers (maximum 4096)")
    if (isinstance(spec.timeout_seconds, bool) or not isinstance(spec.timeout_seconds, (int, float)) or
            not math.isfinite(spec.timeout_seconds) or spec.timeout_seconds <= 0):
        raise ValueError("timeout must be a finite positive number")
    if any(type(value) is not bool for value in (spec.save_raw, spec.reuse_work_dir,
                                                 spec.allow_environment_mismatch)):
        raise ValueError("runner policy fields must be booleans")
    if any(not isinstance(value, str) for value in (*spec.launcher_args, *spec.emulator_args)):
        raise ValueError("launcher and emulator arguments must be strings")
    for name, path in (("emulator", spec.emulator), ("game", spec.game),
                       ("baseline", spec.baseline)):
        if not path.exists():
            raise ValueError(f"{name} does not exist: {path}")
    if not spec.emulator.is_file() or not spec.baseline.is_file():
        raise ValueError("emulator and baseline must be files")
    if spec.report.name.casefold().endswith(".kyty-lock"):
        raise ValueError("report path uses the reserved .kyty-lock suffix")
    if spec.report == spec.baseline or spec.work_dir in (spec.report, spec.baseline):
        raise ValueError("work, report, and baseline paths must be distinct")
    artifact_dir = _artifact_directory(spec.report)
    outputs = {spec.report, spec.report.with_suffix(".log"),
               spec.report.with_name(f"{spec.report.stem}.runner.json")}
    owned_files = outputs | {path.with_name(path.name + ".runner-owned") for path in outputs}
    protected = (spec.emulator, spec.game, spec.baseline)
    if any(path in owned_files for path in protected):
        raise ValueError("runner output collides with an input")
    for path in protected:
        if (spec.work_dir == path or spec.work_dir in path.parents or path in spec.work_dir.parents or
                artifact_dir == path or artifact_dir in path.parents or path in artifact_dir.parents):
            raise ValueError("runner-owned directories overlap an input")
    if _contains_owned_flag(spec.emulator_args):
        raise ValueError("emulator arguments cannot override regression-owned options")


def _clean_owned_directory(path: Path, marker_name: str, reuse: bool) -> None:
    marker = path / marker_name
    if path.is_symlink():
        raise ValueError(f"owned output directory cannot be a symlink: {path}")
    if path.exists() and not path.is_dir():
        raise ValueError(f"owned output path is not a directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    entries = list(path.iterdir())
    if entries and (not marker.is_file() or marker.is_symlink()):
        raise ValueError(f"refusing to clean unowned directory: {path}")
    marker.touch(exist_ok=True)
    if reuse:
        return
    for entry in path.iterdir():
        if entry == marker:
            continue
        if entry.is_symlink() or entry.is_file():
            entry.unlink()
        else:
            shutil.rmtree(entry)


def _prepare_outputs(spec: CaseSpec) -> Path:
    spec.report.parent.mkdir(parents=True, exist_ok=True)

    def prepare_file(path: Path) -> None:
        marker = path.with_name(path.name + ".runner-owned")
        if marker.is_symlink() or (marker.exists() and not marker.is_file()):
            raise ValueError(f"invalid ownership marker: {marker}")
        if path.exists() and not marker.is_file():
            raise ValueError(f"refusing to replace unowned output: {path}")
        marker.touch(exist_ok=True)
        path.unlink(missing_ok=True)

    prepare_file(spec.report)
    _clean_owned_directory(_artifact_directory(spec.report), ".runner-owned", reuse=False)
    _clean_owned_directory(spec.work_dir, ".runner-owned", reuse=spec.reuse_work_dir)
    log = spec.report.with_suffix(".log")
    prepare_file(log)
    prepare_file(spec.report.with_name(f"{spec.report.stem}.runner.json"))
    return log


def _write_result(spec: CaseSpec, result: CaseResult) -> None:
    path = spec.report.with_name(f"{spec.report.stem}.runner.json")
    atomic_write_text(path, json.dumps(asdict(result), indent=2) + "\n")


def run_case(spec: CaseSpec) -> CaseResult:
    validate_case(spec)
    log = _prepare_outputs(spec)
    frames = ",".join(str(frame) for frame in sorted(spec.frames))
    command = [str(spec.emulator), *spec.launcher_args, "--game", str(spec.game),
               "--regression-compare", str(spec.baseline), "--regression-report", str(spec.report),
               "--regression-frames", frames, "--regression-test-id", spec.test_id,
               "--regression-save-raw", "true" if spec.save_raw else "false"]
    if spec.allow_environment_mismatch:
        command += ["--regression-allow-environment-mismatch", "true"]
    command += list(spec.emulator_args)

    started = time.monotonic()
    try:
        process_exit, timed_out = run_process(command, spec.work_dir, log, spec.timeout_seconds)
    except OSError as error:
        process_exit, timed_out = FAILURE, False
        message = f"could not start emulator: {error}"
    else:
        if timed_out:
            message = f"timed out after {spec.timeout_seconds:g}s"
        else:
            passed, message = validate_report(spec.report, spec.baseline, spec.frames, spec.test_id,
                                              spec.allow_environment_mismatch)
            if passed and process_exit != 0:
                message = f"passing report but emulator returned {process_exit}"
            elif not passed and process_exit == 0:
                process_exit = FAILURE
    result = CaseResult(spec.test_id, process_exit, message, timed_out,
                        round(time.monotonic() - started, 3), str(spec.report), str(log))
    _write_result(spec, result)
    return result
