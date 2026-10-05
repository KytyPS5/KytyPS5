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


if __name__ == "__main__":
    unittest.main()
