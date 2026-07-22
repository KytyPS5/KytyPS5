"""Strict validation for emulator-generated graphics regression reports."""

from __future__ import annotations

import json
import string
from pathlib import Path, PurePosixPath, PureWindowsPath

SCHEMA_VERSION = 2
MAX_REPORT_BYTES = 4 * 1024 * 1024
MAX_FRAME_BYTES = 512 * 1024 * 1024
MAX_FRAMES = 4096
MAX_ORDINAL = (1 << 64) - 1
_HEX = frozenset(string.hexdigits)
_PROVENANCE_TEXT = ("test_id", "build_id", "gpu_name", "configuration")
_PROVENANCE_NUMBERS = ("gpu_vendor_id", "gpu_device_id", "gpu_driver", "vulkan_api")
_PROVENANCE_FIELDS = (*_PROVENANCE_TEXT, *_PROVENANCE_NUMBERS)
_LAYOUT_FIELDS = ("width", "height", "row_pitch", "byte_size", "format")
_FORMAT_BYTES = {
    "rgba8_unorm": 4, "rgba8_srgb": 4, "bgra8_unorm": 4, "bgra8_srgb": 4,
    "a2b10g10r10_unorm": 4, "a2r10g10b10_unorm": 4, "rgba16_float": 8,
}


def _integer(value: object, *, positive: bool = False) -> bool:
    return type(value) is int and (value > 0 if positive else 0 <= value <= MAX_ORDINAL)


def _digest(value: object) -> bool:
    return isinstance(value, str) and len(value) == 16 and all(char in _HEX for char in value)


def _provenance(value: object, test_id: str) -> bool:
    if not isinstance(value, dict) or value.get("test_id") != test_id:
        return False
    if any(not isinstance(value.get(name), str) or not value[name] or len(value[name]) > 4096
           for name in _PROVENANCE_TEXT):
        return False
    return all(_integer(value.get(name)) and value[name] <= 0xffffffff
               for name in _PROVENANCE_NUMBERS)


def _same_environment(first: dict[str, object], second: dict[str, object]) -> bool:
    fields = ("test_id", "gpu_name", "configuration", *_PROVENANCE_NUMBERS)
    return all(first[name] == second[name] for name in fields)


def _same_provenance(first: dict[str, object], second: dict[str, object]) -> bool:
    return all(first[name] == second[name] for name in _PROVENANCE_FIELDS)


def _same_digest(first: object, second: object) -> bool:
    return _digest(first) and _digest(second) and first.casefold() == second.casefold()


def _valid_layout(record: dict[str, object]) -> bool:
    bytes_per_pixel = _FORMAT_BYTES.get(record.get("format"))
    return (bytes_per_pixel is not None and
            _integer(record.get("width"), positive=True) and
            _integer(record.get("height"), positive=True) and
            _integer(record.get("row_pitch"), positive=True) and
            _integer(record.get("byte_size"), positive=True) and
            record["row_pitch"] == record["width"] * bytes_per_pixel and
            record["byte_size"] == record["row_pitch"] * record["height"] and
            record["byte_size"] <= MAX_FRAME_BYTES)


def _safe_artifact(value: object) -> bool:
    if not isinstance(value, str) or not value or len(value) > 1024:
        return False
    windows = PureWindowsPath(value)
    posix = PurePosixPath(value)
    return (not windows.is_absolute() and not windows.drive and not posix.is_absolute() and
            ".." not in windows.parts and ".." not in posix.parts)


def _read_object(path: Path, kind: str) -> tuple[dict[str, object] | None, str]:
    try:
        if path.stat().st_size > MAX_REPORT_BYTES:
            return None, f"{kind} exceeds the size limit"
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return None, f"cannot read a valid {kind}: {error}"
    if not isinstance(value, dict):
        return None, f"{kind} root must be an object"
    return value, ""


def _load_baseline(path: Path, frames: list[int], test_id: str) -> tuple[
        dict[str, object] | None, dict[int, dict[str, object]] | None, str]:
    baseline, error = _read_object(path, "baseline")
    if baseline is None:
        return None, None, error
    provenance = baseline.get("provenance")
    records = baseline.get("frames")
    if (baseline.get("schema_version") != SCHEMA_VERSION or
            baseline.get("mode") != "record" or baseline.get("algorithm") != "xxh3_128" or
            baseline.get("complete") is not True or baseline.get("passed") is not True or
            not _provenance(provenance, test_id) or not isinstance(records, list) or
            not records or len(records) > MAX_FRAMES):
        return None, None, "baseline metadata is invalid or incomplete"
    by_ordinal: dict[int, dict[str, object]] = {}
    for record in records:
        if (not isinstance(record, dict) or not _integer(record.get("ordinal")) or
                record.get("status") != "recorded" or not _valid_layout(record) or
                not _digest(record.get("hash_low")) or not _digest(record.get("hash_high")) or
                any(name in record and not _safe_artifact(record[name])
                    for name in ("raw_file", "image_file")) or
                record["ordinal"] in by_ordinal):
            return None, None, "baseline contains an invalid frame record"
        by_ordinal[record["ordinal"]] = record
    if any(ordinal not in by_ordinal for ordinal in frames):
        return None, None, "baseline does not contain every requested frame"
    return provenance, by_ordinal, ""


def validate_report(path: Path, baseline_path: Path, frames: list[int], test_id: str,
                    allow_environment_mismatch: bool = False) -> tuple[bool, str]:
    baseline_provenance, baseline_frames, error = _load_baseline(baseline_path, frames, test_id)
    if baseline_provenance is None or baseline_frames is None:
        return False, error
    report, error = _read_object(path, "report")
    if report is None:
        return False, error
    if (report.get("schema_version") != SCHEMA_VERSION or report.get("mode") != "compare" or
            report.get("algorithm") != "xxh3_128" or report.get("complete") is not True or
            type(report.get("passed")) is not bool):
        return False, "report metadata is invalid or incomplete"
    reported_baseline = report.get("baseline_provenance")
    run_provenance = report.get("run_provenance")
    if (not _provenance(reported_baseline, test_id) or
            not _provenance(run_provenance, test_id) or
            type(report.get("environment_match")) is not bool or
            not _same_provenance(reported_baseline, baseline_provenance)):
        return False, "report provenance is invalid or does not match the baseline"
    environments_match = _same_environment(reported_baseline, run_provenance)
    if report["environment_match"] != environments_match:
        return False, "report environment result is inconsistent"
    if not environments_match and not allow_environment_mismatch:
        return False, "report environment does not match the baseline"

    records = report.get("frames")
    if not isinstance(records, list) or len(records) != len(frames):
        return False, "report does not contain exactly the requested frames"
    for record, ordinal in zip(records, sorted(frames), strict=True):
        if (not isinstance(record, dict) or not _integer(record.get("ordinal")) or
                record["ordinal"] != ordinal):
            return False, "report does not contain exactly the requested frames"
        if record.get("status") == "match":
            baseline_record = baseline_frames[ordinal]
            if (not _valid_layout(record) or not _digest(record.get("hash_low")) or
                    not _digest(record.get("hash_high")) or
                    not _digest(record.get("expected_hash_low")) or
                    not _digest(record.get("expected_hash_high"))):
                return False, "report contains an invalid matching frame"
            if (not _same_digest(record["hash_low"], record["expected_hash_low"]) or
                    not _same_digest(record["hash_high"], record["expected_hash_high"])):
                return False, "matching frame hashes are inconsistent"
            if (not _same_digest(record["expected_hash_low"], baseline_record["hash_low"]) or
                    not _same_digest(record["expected_hash_high"], baseline_record["hash_high"]) or
                    any(record[name] != baseline_record[name] for name in _LAYOUT_FIELDS)):
                return False, "matching frame does not match the baseline"
    passed = report["passed"] and all(record.get("status") == "match" for record in records)
    return passed, "all selected frames match" if passed else "one or more frames differ"
