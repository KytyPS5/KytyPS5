"""Summarize opt-in Kyty CPU presentation traces; uses only the standard library."""
import argparse
import csv
import json
import statistics
from pathlib import Path

PREPARATION_FIELDS = (
    "prepare_wait_ms", "prepare_lock_ms", "resolve_ms", "inputs_ms",
    "fg_capture_ms", "dlss_record_ms",
)


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lo = int(index)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (index - lo)


def summarize(path, start, end):
    rows = []
    discarded = 0
    with path.open(newline="", encoding="utf-8") as trace:
        reader = csv.DictReader(trace)
        optional = [key for key in (*PREPARATION_FIELDS, "display_frames")
                    if key in (reader.fieldnames or ())]
        for raw in reader:
            try:
                row = {key: float(raw[key]) for key in (
                    "elapsed_ms", "frame_ms", "present_ms", "new_frame", "dlss_evaluated", *optional
                )}
            except (KeyError, TypeError, ValueError):
                # An interrupted process can leave one incomplete buffered row.
                discarded += 1
                continue
            if start * 1000 <= row["elapsed_ms"] <= end * 1000:
                rows.append(row)
    if len(rows) < 2:
        raise ValueError(f"{path}: fewer than two samples in the selected interval")
    elapsed = (rows[-1]["elapsed_ms"] - rows[0]["elapsed_ms"]) / 1000
    # The first selected row closes an interval that starts outside the window.
    intervals = [row["frame_ms"] for row in rows[1:] if row["frame_ms"] > 0]
    present = [row["present_ms"] for row in rows]
    new_frames = sum(int(row["new_frame"]) for row in rows)
    result = {
        "trace": str(path), "seconds": round(elapsed, 3), "submissions": len(rows),
        "submission_fps": round((len(rows) - 1) / elapsed, 2),
        "new_frames": new_frames,
        "guest_fps": round((new_frames - int(rows[0]["new_frame"])) / elapsed, 2),
        "dlss_evaluations": sum(int(row["new_frame"] and row["dlss_evaluated"]) for row in rows),
        "interval_ms": {
            "median": round(statistics.median(intervals), 3),
            "p95": round(percentile(intervals, .95), 3),
            "p99": round(percentile(intervals, .99), 3),
            "max": round(max(intervals), 3),
            "over_16_67": sum(value > 1000 / 60 for value in intervals),
            "over_33_33": sum(value > 1000 / 30 for value in intervals),
        },
        "present_ms": {
            "median": round(statistics.median(present), 3),
            "p99": round(percentile(present, .99), 3),
            "max": round(max(present), 3),
        },
        "incomplete_rows": discarded,
    }
    prepared = [row for row in rows if row["new_frame"] and
                any(row.get(key, 0) > 0 for key in PREPARATION_FIELDS)]
    if prepared:
        result["preparation_ms"] = {
            key.removesuffix("_ms"): {
                "median": round(statistics.median(row[key] for row in prepared), 3),
                "p99": round(percentile([row[key] for row in prepared], .99), 3),
            }
            for key in PREPARATION_FIELDS if key in optional
        }
    if "display_frames" in optional:
        result["sdk_display_fps"] = round(
            sum(row["display_frames"] for row in rows[1:] if row["new_frame"]) / elapsed, 2
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--start", type=float, default=0, help="Start time in seconds")
    parser.add_argument("--end", type=float, default=float("inf"), help="End time in seconds")
    args = parser.parse_args()
    if args.start < 0 or args.end <= args.start:
        parser.error("use 0 <= start < end")
    print(json.dumps([summarize(path, args.start, args.end) for path in args.traces], indent=2))


if __name__ == "__main__":
    main()
