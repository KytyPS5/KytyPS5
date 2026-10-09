"""Regression checks for selected CPU presentation intervals."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "frame_analysis", Path(__file__).resolve().parents[1] / "tools/analyze-frame-timings.py"
)
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class FrameTimingAnalysisTests(unittest.TestCase):
    def summarize_rows(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            path.write_text(
                "elapsed_ms,frame_ms,present_ms,new_frame,dlss_evaluated\n" + rows,
                encoding="utf-8",
            )
            return analysis.summarize(path, 0, float("inf"))

    def test_non_finite_measurements_are_rejected(self):
        for value in ("nan", "inf", "-inf"):
            for column in range(5):
                with self.subTest(value=value, column=column):
                    fields = ["10", "10", "1", "1", "0"]
                    fields[column] = value
                    with self.assertRaisesRegex(ValueError, "non-finite"):
                        self.summarize_rows(
                            "0,0,1,1,0\n" + ",".join(fields) + "\n20,10,1,1,0\n"
                        )

    def test_zero_or_negative_elapsed_time_is_rejected(self):
        for rows in ("10,10,1,1,0\n10,10,1,1,0\n",
                     "20,10,1,1,0\n10,10,1,1,0\n"):
            with self.subTest(rows=rows):
                with self.assertRaisesRegex(ValueError, "positive elapsed time"):
                    self.summarize_rows(rows)

    def test_no_positive_intervals_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "no positive frame intervals"):
            self.summarize_rows("0,0,1,1,0\n10,0,1,1,0\n20,-1,1,1,0\n")

    def test_transition_before_window_does_not_count_as_stall(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            path.write_text(
                "frame,elapsed_ms,frame_ms,present_ms,new_frame,dlss_evaluated\n"
                "1,0,0,1,1,0\n"
                "2,35000,35000,2,1,1\n"
                "3,35010,10,3,1,1\n"
                "4,35020,10,4,1,1\n",
                encoding="utf-8",
            )
            result = analysis.summarize(path, 35, 36)
        self.assertEqual(result["guest_fps"], 100)
        self.assertEqual(result["interval_ms"]["max"], 10)
        self.assertEqual(result["interval_ms"]["p99"], 10)
        self.assertEqual(result["interval_ms"]["over_33_33"], 0)
        self.assertEqual(result["present_ms"]["median"], 3)

    def test_stall_fully_inside_window_is_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            path.write_text(
                "frame,elapsed_ms,frame_ms,present_ms,new_frame,dlss_evaluated\n"
                "1,35000,25000,1,1,0\n"
                "2,35010,10,2,1,0\n"
                "3,35110,100,3,1,0\n"
                "4,35120,10,4,1,0\n"
                "5,35130,\n",
                encoding="utf-8",
            )
            result = analysis.summarize(path, 35, 36)
        self.assertEqual(result["interval_ms"]["median"], 10)
        self.assertEqual(result["interval_ms"]["max"], 100)
        self.assertEqual(result["interval_ms"]["over_33_33"], 1)
        self.assertEqual(result["incomplete_rows"], 1)

    def test_preparation_and_display_counts_exclude_cached_presents(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            path.write_text(
                "frame,elapsed_ms,frame_ms,present_ms,new_frame,dlss_evaluated,"
                "prepare_wait_ms,prepare_lock_ms,resolve_ms,inputs_ms,"
                "fg_capture_ms,dlss_record_ms,display_frames\n"
                "1,35000,20000,1,1,1,2,1,0.1,0.2,0.3,0.4,2\n"
                "2,35005,5,1,0,1,0,0,0,0,0,0,0\n"
                "3,35010,5,1,1,1,4,1,0.1,0.2,0.3,0.4,2\n"
                "4,35020,10,1,1,1,6,1,0.1,0.2,0.3,0.4,2\n"
                "5,35030,10,1,1,1,8,\n",
                encoding="utf-8",
            )
            result = analysis.summarize(path, 35, 36)
        self.assertEqual(result["guest_fps"], 100)
        self.assertEqual(result["sdk_display_fps"], 200)
        self.assertEqual(result["preparation_ms"]["prepare_wait"]["median"], 4)
        self.assertEqual(result["incomplete_rows"], 1)


if __name__ == "__main__":
    unittest.main()
