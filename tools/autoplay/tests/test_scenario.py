"""Scenario files, emulator command lines, and screenshot matching."""
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import kyty_autoplay as ka  # noqa: E402
try:
    from PIL import Image, ImageDraw  # noqa: E402
    HAVE_PILLOW = True
except ImportError:  # the harness itself reports a missing Pillow when it needs it
    HAVE_PILLOW = False


class Durations(unittest.TestCase):
    def test_units(self):
        self.assertEqual(ka.parse_duration("5s"), 5.0)
        self.assertEqual(ka.parse_duration("250ms"), 0.25)
        self.assertEqual(ka.parse_duration("2m"), 120.0)
        self.assertEqual(ka.parse_duration("1h"), 3600.0)
        self.assertEqual(ka.parse_duration(7), 7.0)
        self.assertEqual(ka.parse_duration("1.5"), 1.5)

    def test_rejects_garbage(self):
        for bad in ("soon", "", "5 parsecs", "-3s"):
            with self.assertRaises(ka.HarnessError):
                ka.parse_duration(bad)


class CommandLine(unittest.TestCase):
    def test_strip_options_handles_both_spellings(self):
        args = ["--game", "g", "--printf-direction", "Console", "--gpu=1", "--automation-dir=/x", "--fullscreen"]
        self.assertEqual(ka.strip_options(args, {"--printf-direction", "--automation-dir"}),
                         ["--game", "g", "--gpu=1", "--fullscreen"])

    def test_harness_flags_win_over_the_users(self):
        run_dir = Path("/runs/1")
        command = ka.build_command(Path("/b/kyty_emulator"), run_dir,
                                   ["--game", "/g", "--printf-direction", "Console",
                                    "--shader-capture-dir", "/elsewhere", "--gpu", "0"], 7)
        self.assertEqual(command[0], "/b/kyty_emulator")
        self.assertEqual(command.count("--printf-direction"), 1)
        self.assertEqual(command[command.index("--printf-direction") + 1], "File")
        self.assertEqual(command[command.index("--shader-capture-dir") + 1], "/runs/1/capture")
        self.assertEqual(command[command.index("--automation-dir") + 1], "/runs/1")
        self.assertEqual(command[command.index("--automation-shot-interval") + 1], "7")
        self.assertIn("--gpu", command)
        self.assertNotIn("/elsewhere", command)

    def test_parse_run_script(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "kyty_run.sh"
            script.write_text('#!/bin/sh\ncd "$(dirname "$0")"\n'
                              '"./kyty_emulator" --game "/games/My Game" --gpu 0 --present-mode Fifo\n'
                              'echo "Press any key..."\nread x\n')
            self.assertEqual(ka.parse_run_script(script),
                             ["--game", "/games/My Game", "--gpu", "0", "--present-mode", "Fifo"])
            script.write_text("echo nothing here\n")
            with self.assertRaises(ka.HarnessError):
                ka.parse_run_script(script)


class WhileWaiting(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(ka.parse_while_waiting("press cross every 3s"), [("press cross", 3.0)])
        self.assertEqual(ka.parse_while_waiting(["press cross every 3s", "stick l 0 1 500 every 10s"]),
                         [("press cross", 3.0), ("stick l 0 1 500", 10.0)])
        self.assertEqual(ka.parse_while_waiting(None), [])

    def test_rejects_missing_interval(self):
        with self.assertRaises(ka.HarnessError):
            ka.parse_while_waiting("press cross")


@unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
class ScenarioFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "refs").mkdir()
        Image.new("RGB", (8, 8), (255, 255, 255)).save(self.dir / "refs" / "a.png")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, text: str) -> Path:
        path = self.dir / "s.toml"
        path.write_text(text)
        return path

    def test_loads_every_kind_of_step(self):
        scenario = ka.load_scenario(self.write('''
name = "demo"
game = "/games/x"
[run]
timeout = "10m"
[soak]
script = ["stick l 0 1 1000"]
[[step]]
wait = "5s"
[[step]]
wait_frames = 120
[[step]]
press = "cross"
ms = 200
[[step]]
send = "stick l 0 1 500"
[[step]]
checkpoint = "menu"
[[step]]
until = { ref = "refs/a.png", box = [0.0, 0.5, 0.5, 1.0], max_diff = 0.1, timeout = "30s", while_waiting = "press cross every 3s" }
[[step]]
verify = "controllable"
radar = { ref = "refs/a.png", box = [0.0, 0.5, 0.5, 1.0] }
stick = "l 0 1 3000"
'''))
        self.assertEqual(scenario.name, "demo")
        self.assertEqual(scenario.game, "/games/x")
        self.assertEqual([s.action for s in scenario.steps],
                         ["wait", "wait_frames", "press", "send", "checkpoint", "until", "verify"])
        self.assertEqual(scenario.steps[2].options, {"ms": 200})
        self.assertEqual(scenario.run["timeout"], "10m")
        self.assertEqual(scenario.soak["script"], ["stick l 0 1 1000"])

    def test_no_scenario_means_no_steps(self):
        self.assertEqual(ka.load_scenario(None).steps, [])

    def test_empty_scenario_is_valid(self):
        self.assertEqual(ka.load_scenario(self.write('name = "empty"\n')).steps, [])

    def test_errors_are_harness_errors(self):
        bad = {
            "two actions": '[[step]]\nwait = "1s"\npress = "cross"\n',
            "no action": '[[step]]\nms = 5\n',
            "missing ref": '[[step]]\nuntil = { ref = "refs/nope.png" }\n',
            "bad duration": '[[step]]\nwait = "soon"\n',
            "bad frames": '[[step]]\nwait_frames = 0\n',
            "bad verify": '[[step]]\nverify = "fun"\n',
            "verify without radar": '[[step]]\nverify = "controllable"\n',
            "bad while_waiting": '[[step]]\nuntil = { ref = "refs/a.png", while_waiting = "mash" }\n',
            "not toml": "[[step\n",
        }
        for label, text in bad.items():
            with self.subTest(label):
                with self.assertRaises(ka.HarnessError):
                    ka.load_scenario(self.write(text))

    def test_missing_file(self):
        with self.assertRaises(ka.HarnessError):
            ka.load_scenario(self.dir / "absent.toml")

    def test_shipped_scenarios_load(self):
        shipped = HERE.parent / "scenarios"
        for path in sorted(shipped.glob("*.toml")):
            with self.subTest(path.name):
                ka.load_scenario(path)


@unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
class Images(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def save(self, name, image):
        path = self.dir / name
        image.save(path)
        return path

    def test_crop_fraction(self):
        image = Image.new("RGB", (200, 100))
        self.assertEqual(ka.crop_fraction(image, [0.0, 0.5, 0.25, 1.0]).size, (50, 50))
        with self.assertRaises(ka.HarnessError):
            ka.crop_fraction(image, [0.5, 0.5, 0.2, 0.9])
        with self.assertRaises(ka.HarnessError):
            ka.crop_fraction(image, [0.0, 0.0, 1.5, 1.0])

    def test_mean_abs_diff_extremes(self):
        black, white = [0] * 16, [255] * 16
        self.assertEqual(ka.mean_abs_diff(black, black), 0.0)
        self.assertEqual(ka.mean_abs_diff(black, white), 1.0)
        self.assertAlmostEqual(ka.mean_abs_diff([0, 0], [255, 0]), 0.5)

    def frame(self, radar=True):
        image = Image.new("RGB", (320, 180), (30, 30, 60))
        if radar:
            draw = ImageDraw.Draw(image)
            draw.rectangle((6, 120, 66, 175), fill=(10, 40, 10))
            draw.ellipse((14, 126, 58, 170), fill=(220, 240, 220))
        return image

    def test_match_crop_finds_the_radar_only_when_it_is_there(self):
        box = [0.0, 0.62, 0.22, 1.0]
        with_radar = self.save("with.png", self.frame(True))
        without = self.save("without.png", self.frame(False))
        reference = self.dir / "ref.png"
        ka.crop_fraction(self.frame(True), box).save(reference)
        self.assertLess(ka.match_crop(with_radar, reference, box), 0.02)
        self.assertGreater(ka.match_crop(without, reference, box), 0.15)

    def test_match_tolerates_a_different_resolution(self):
        box = [0.0, 0.62, 0.22, 1.0]
        reference = self.dir / "ref.png"
        ka.crop_fraction(self.frame(True), box).save(reference)
        big = self.save("big.png", self.frame(True).resize((1280, 720)))
        self.assertLess(ka.match_crop(big, reference, box), 0.05)

    def test_frame_diff(self):
        a = self.save("a.png", self.frame(True))
        self.assertEqual(ka.frame_diff(a, a), 0.0)
        shifted = Image.new("RGB", (320, 180), (30, 30, 60))
        ImageDraw.Draw(shifted).ellipse((200, 20, 250, 70), fill=(255, 255, 255))
        self.assertGreater(ka.frame_diff(a, self.save("b.png", shifted)), 0.01)


if __name__ == "__main__":
    unittest.main()
