"""End to end: the real harness driving a fake emulator that speaks the automation protocol."""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
HARNESS = HERE.parent / "kyty_autoplay.py"
FAKE = HERE / "fake_emulator.py"
FIXTURE_CAPTURE = REPO / "tests" / "data" / "shader_capture" / "gpu_selected_store"

sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

try:
    import fake_emulator  # noqa: E402
    import kyty_autoplay as ka  # noqa: E402
    HAVE_PILLOW = True
except ImportError:
    HAVE_PILLOW = False


def real_emulator():
    """The built kyty_emulator, for the tests that replay a shader, or None."""
    candidates = [os.environ.get("KYTY_EMULATOR"), str(REPO / "_Build" / "linux" / "kyty_emulator")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


@unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
class FakeRunTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        # The harness launches an executable; this one runs the fake with the current Python.
        self.emulator = self.build / "kyty_emulator"
        self.emulator.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{FAKE}" "$@"\n')
        self.emulator.chmod(0o755)
        self.runs = self.root / "runs"
        self.env = {**os.environ, "KYTY_AUTOPLAY_STOP_GRACE": "1.5"}
        self.addCleanup(self.kill_strays)

    def tearDown(self):
        self.tmp.cleanup()

    def kill_strays(self):
        for meta in self.runs.glob("*/meta.json") if self.runs.exists() else []:
            try:
                pid = json.loads(meta.read_text())["pid"]
                os.killpg(pid, signal.SIGKILL)
            except (OSError, ValueError, KeyError):
                pass

    # ---- helpers ----------------------------------------------------------------------------
    def cli(self, *args, behavior="pass", extra_env=None, timeout=180):
        env = {**self.env, "FAKE_BEHAVIOR": behavior, **(extra_env or {})}
        command = [sys.executable, str(HARNESS), args[0],
                   "--build-dir", str(self.build), "--emulator", str(self.emulator),
                   "--root", str(self.runs), *args[1:]]
        return subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)

    def run_dirs(self):
        return sorted(self.runs.glob("*/")) if self.runs.exists() else []

    def result(self, run_dir=None):
        run_dir = run_dir or self.run_dirs()[-1]
        return json.loads((run_dir / "result.json").read_text())

    def run_game(self, scenario_text="", *args, behavior="pass", extra_env=None):
        scenario = self.root / "s.toml"
        scenario.write_text(scenario_text or 'name = "empty"\n')
        return self.cli("run", "--game", "/games/fake", "--scenario", str(scenario), *args,
                        behavior=behavior, extra_env=extra_env)

    def make_refs(self):
        refs = self.root / "refs"
        refs.mkdir(exist_ok=True)
        ka.crop_fraction(fake_emulator.render_menu(), [0.3, 0.3, 0.7, 0.7]).save(refs / "menu.png")
        ka.crop_fraction(fake_emulator.render_gameplay(0), [0.0, 0.62, 0.22, 1.0]).save(refs / "radar.png")

    PLAY = '''
name = "fake story"
[[step]]
wait = "0.3s"
[[step]]
until = { ref = "refs/menu.png", box = [0.3, 0.3, 0.7, 0.7], max_diff = 0.05, timeout = "15s" }
[[step]]
checkpoint = "menu"
[[step]]
until = { ref = "refs/radar.png", box = [0.0, 0.62, 0.22, 1.0], max_diff = 0.08, timeout = "%(radar_timeout)s", while_waiting = "press cross every 1s" }
[[step]]
checkpoint = "gameplay"
%(verify)s
'''
    VERIFY = '''
[[step]]
verify = "controllable"
radar = { ref = "refs/radar.png", box = [0.0, 0.62, 0.22, 1.0], max_diff = 0.08 }
stick = "l 0 1 1500"
idle = "1s"
'''

    def play(self, verify=True, radar_timeout="20s"):
        return self.PLAY % {"radar_timeout": radar_timeout, "verify": self.VERIFY if verify else ""}

    # ---- PASS -------------------------------------------------------------------------------
    def test_pass_when_controllable_and_soak_finishes(self):
        self.make_refs()
        done = self.run_game(self.play(), "--soak", "6", "--timeout", "90", "--boot-grace", "30")
        self.assertEqual(done.returncode, 0, done.stderr)
        run = self.run_dirs()[-1]
        result = self.result()
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["checkpoints"], ["menu", "gameplay"])
        self.assertEqual(result["furthest_checkpoint"], "gameplay")
        for name in ("cmd.txt", "stdout.txt", "_kyty.txt", "status.jsonl", "result.json", "summary.md",
                     "exit.json", "progress.json", "commands.txt", "events.jsonl"):
            self.assertTrue((run / name).exists(), name)
        self.assertTrue((run / "shots" / "radar_ok.png").exists())
        commands = (run / "commands.txt").read_text()
        self.assertIn("press cross", commands)
        self.assertIn("stick l 0 1 1500", commands)   # the controllability check
        self.assertGreaterEqual(commands.count("stick l"), 3)  # plus the soak's movement
        self.assertEqual(commands.strip().splitlines()[-1], "quit")
        self.assertEqual(json.loads((run / "exit.json").read_text())["returncode"], 0)
        self.assertTrue(json.loads((run / "progress.json").read_text())["passed"])
        self.assertGreater(len((run / "status.jsonl").read_text().splitlines()), 3)

    def test_pass_even_if_the_emulator_ignores_quit(self):
        self.make_refs()
        done = self.run_game(self.play(), "--timeout", "90", "--boot-grace", "30", behavior="ignore_quit")
        self.assertEqual(done.returncode, 0, done.stderr)
        result = self.result()
        self.assertEqual(result["result"], "PASS")
        self.assertIn("killed", result["reason"])

    # ---- not controllable -------------------------------------------------------------------
    def test_a_frozen_world_is_not_controllable(self):
        self.make_refs()
        done = self.run_game(self.play(), "--timeout", "90", "--boot-grace", "30", behavior="frozen_world")
        self.assertEqual(done.returncode, 13, done.stderr)
        result = self.result()
        self.assertEqual(result["result"], "STUCK")
        self.assertIn("not controllable", result["reason"])
        self.assertEqual(result["checkpoints"], ["menu", "gameplay"])

    def test_stuck_in_the_menu(self):
        self.make_refs()
        done = self.run_game(self.play(radar_timeout="3s"), "--timeout", "90", "--boot-grace", "30",
                             behavior="stuck")
        self.assertEqual(done.returncode, 13, done.stderr)
        result = self.result()
        self.assertEqual(result["result"], "STUCK")
        self.assertIn("radar.png", result["reason"])
        self.assertEqual(result["furthest_checkpoint"], "menu")
        self.assertTrue(Path(result["last_shot"]).exists())

    def test_scenario_without_a_verify_step_cannot_pass(self):
        self.make_refs()
        done = self.run_game(self.play(verify=False), "--timeout", "90", "--boot-grace", "30")
        self.assertEqual(done.returncode, 13, done.stderr)
        self.assertIn("verify", self.result()["reason"])

    def test_run_timeout_with_no_scenario(self):
        done = self.run_game("", "--timeout", "3", "--boot-grace", "30")
        self.assertEqual(done.returncode, 13, done.stderr)
        self.assertIn("timeout", self.result()["reason"])

    # ---- aborts -----------------------------------------------------------------------------
    def test_shader_abort_is_replayed_and_disassembled(self):
        emulator = real_emulator()
        fatal = self.root / "fatal.txt"
        env = {"FAKE_FATAL_FILE": str(fatal), "FAKE_CAPTURE_DIR": str(FIXTURE_CAPTURE)}
        if emulator is not None:
            # What the game would print: the real recompiler's failure on the fixture capture.
            copy = self.root / "fixture_copy"
            shutil.copytree(FIXTURE_CAPTURE, copy)
            replay = subprocess.run([str(emulator), "--shader-replay", str(copy), "--no-dump"],
                                    capture_output=True, text=True)
            self.assertEqual(replay.returncode, 65, replay.stdout)
            fatal.write_text(replay.stdout)
        else:
            fatal.write_text((HERE / "fixtures" / "shader_abort.stdout.txt").read_text())
            env.pop("FAKE_CAPTURE_DIR")
        extra = ["--replay-emulator", str(emulator)] if emulator else []
        done = self.run_game("", *extra, "--timeout", "60", behavior="shader_abort", extra_env=env)
        self.assertEqual(done.returncode, 10, done.stderr)
        result = self.result()
        self.assertEqual(result["result"], "SHADER_ABORT")
        self.assertEqual(result["returncode"], 65)
        self.assertIn("GPU-selected access requires a raw DWORD", result["reason"])
        shader = result["shader"]
        if emulator is None:
            self.assertEqual(shader["hash"], "0x6a53456e7ef5d1b0")
            self.assertEqual(shader["pc"], "0x86c")
            self.skipTest("kyty_emulator is not built; replay checks skipped")
        self.assertEqual(shader["hash"], "0x00000000000000b2")
        self.assertEqual(len(shader["captures"]), 1)
        replay = shader["replay"]
        self.assertEqual(replay["replay_exit"], 65)
        self.assertTrue(replay["reproduces"], replay)
        self.assertTrue(any(line.startswith("=> ") and "BUFFER_STORE_DWORD" in line
                            for line in replay["disasm_window"]), replay["disasm_window"])
        summary = (self.run_dirs()[-1] / "summary.md").read_text()
        self.assertIn("SHADER_ABORT", summary)
        self.assertIn("replay reproduces the game failure: **yes**", summary)
        self.assertIn("BUFFER_STORE_DWORD", summary)

    def test_other_abort(self):
        done = self.run_game("", "--timeout", "60", behavior="other_abort")
        self.assertEqual(done.returncode, 11, done.stderr)
        result = self.result()
        self.assertEqual(result["result"], "OTHER_ABORT")
        self.assertIn("Not implemented (a.b.c)", result["reason"])
        self.assertNotIn("shader", result)

    def test_crash_by_signal(self):
        done = self.run_game("", "--timeout", "60", behavior="segv")
        self.assertEqual(done.returncode, 11, done.stderr)
        result = self.result()
        self.assertEqual(result["signal"], signal.SIGSEGV)
        self.assertIn("SIGSEGV", result["reason"])

    def test_exiting_early_is_an_abort(self):
        done = self.run_game("", "--timeout", "60", behavior="exit0")
        self.assertEqual(done.returncode, 11, done.stderr)
        self.assertIn("before the scenario finished", self.result()["reason"])

    # ---- hangs ------------------------------------------------------------------------------
    def test_hang_saves_backtraces_and_the_last_heartbeat(self):
        done = self.run_game("", "--timeout", "120", "--hang-seconds", "2", "--boot-grace", "20",
                             behavior="hang")
        self.assertEqual(done.returncode, 12, done.stderr)
        run = self.run_dirs()[-1]
        result = self.result()
        self.assertEqual(result["result"], "HANG")
        self.assertIn("present count flat", result["reason"])
        self.assertGreater(result["hang"]["host_presents"], 0)
        self.assertEqual(result["hang"]["schedulers"],
                         [{"gpu_tick": 7, "current_tick": 9, "gpu_behind_by": 2}])
        self.assertEqual(result["hang"]["recent_submits"][0]["tick"], 9)
        self.assertEqual(result["hang"]["recent_submits"][0]["op_name"], "EopWrite")
        self.assertEqual(result["hang"]["last_shader"]["hash"], "0x00000000000000b2")
        self.assertTrue((run / "gdb.txt").exists())
        self.assertGreater(len((run / "gdb.txt").read_text()), 0)
        pid = json.loads((run / "meta.json").read_text())["pid"]
        with self.assertRaises(ProcessLookupError):
            os.killpg(pid, 0)                      # nothing is left running

    def test_never_presenting_is_a_hang_after_the_boot_grace(self):
        done = self.run_game("", "--timeout", "120", "--hang-seconds", "2", "--boot-grace", "3",
                             behavior="boot_hang")
        self.assertEqual(done.returncode, 12, done.stderr)
        self.assertIn("before the first present", self.result()["reason"])

    def test_interrupting_the_harness_does_not_leave_the_emulator_running(self):
        scenario = self.root / "s.toml"
        scenario.write_text('name = "empty"\n')
        command = [sys.executable, str(HARNESS), "run", "--build-dir", str(self.build),
                   "--emulator", str(self.emulator), "--root", str(self.runs), "--game", "/games/fake",
                   "--scenario", str(scenario), "--timeout", "120"]
        harness = subprocess.Popen(command, env={**self.env, "FAKE_BEHAVIOR": "pass"},
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and not list(self.runs.glob("*/status.json")):
                time.sleep(0.1)
            pid = json.loads(next(self.runs.glob("*/meta.json")).read_text())["pid"]
            os.killpg(pid, 0)                       # running
            harness.send_signal(signal.SIGINT)
            harness.wait(timeout=30)
        finally:
            if harness.poll() is None:
                harness.kill()
        self.assertEqual(harness.returncode, 130)
        with self.assertRaises(ProcessLookupError):
            os.killpg(pid, 0)

    # ---- harness errors ---------------------------------------------------------------------
    def test_errors_are_exit_2(self):
        missing = self.cli("run", "--game", "/games/fake", "--emulator", str(self.build / "no_such_emulator"))
        self.assertEqual(missing.returncode, 2)
        self.assertIn("does not exist", missing.stderr)
        bad = self.run_game("[[step\n")
        self.assertEqual(bad.returncode, 2)
        self.assertIn("not valid TOML", bad.stderr)
        nogame = self.cli("run")
        self.assertEqual(nogame.returncode, 2)
        self.assertIn("no game given", nogame.stderr)
        self.assertEqual(self.run_dirs(), [])        # nothing was launched, so there is no run

    def test_a_scenario_command_the_emulator_rejects_is_a_harness_error(self):
        done = self.run_game('[[step]]\nsend = "dance wildly"\n[[step]]\nwait = "5s"\n',
                             "--timeout", "60", "--boot-grace", "30")
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertEqual(self.result()["result"], "HARNESS_ERROR")
        self.assertIn("dance wildly", self.result()["reason"])

    # ---- interactive mode -------------------------------------------------------------------
    def test_interactive_session(self):
        started = self.cli("start", "--game", "/games/fake", "--wait", "30")
        self.assertEqual(started.returncode, 0, started.stderr)
        run = Path(json.loads(started.stdout)["run_dir"])
        self.assertTrue((run / "status.json").exists())

        ack = self.cli("send", "press cross")
        self.assertEqual(ack.returncode, 0, ack.stderr)
        self.assertTrue(json.loads(ack.stdout)["ok"])
        self.cli("send", "press cross")

        rejected = self.cli("send", "dance")
        self.assertEqual(rejected.returncode, 2)
        self.assertFalse(json.loads(rejected.stdout)["ok"])   # reported, and the session survives

        shot = self.cli("shot", "menu_check")
        self.assertEqual(shot.returncode, 0, shot.stderr)
        path = Path(shot.stdout.strip())
        self.assertTrue(path.exists())
        self.assertEqual(path.name, "menu_check.png")
        from PIL import Image
        with Image.open(path) as image:
            self.assertEqual(image.size, (fake_emulator.WIDTH, fake_emulator.HEIGHT))

        status = json.loads(self.cli("status").stdout)
        self.assertTrue(status["running"])
        self.assertGreater(status["status"]["host_presents"], 0)

        stopped = self.cli("stop")
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        self.assertEqual(json.loads(stopped.stdout)["result"], "STOPPED")
        self.assertEqual(json.loads((run / "exit.json").read_text())["returncode"], 0)
        after = json.loads(self.cli("status", "--run", str(run)).stdout)
        self.assertFalse(after["running"])

    def test_start_reports_an_early_death(self):
        started = self.cli("start", "--game", "/games/fake", "--wait", "30", behavior="other_abort")
        self.assertEqual(started.returncode, 11, started.stderr)
        self.assertEqual(json.loads(started.stdout)["result"], "OTHER_ABORT")

    # ---- classify and ref -------------------------------------------------------------------
    def test_classify_rebuilds_a_run(self):
        done = self.run_game("", "--timeout", "60", behavior="other_abort")
        self.assertEqual(done.returncode, 11)
        run = self.run_dirs()[-1]
        before = self.result()
        (run / "result.json").unlink()
        (run / "summary.md").unlink()
        again = self.cli("classify", str(run), "--no-replay")
        self.assertEqual(again.returncode, 11, again.stderr)
        self.assertEqual(self.result()["reason"], before["reason"])
        self.assertTrue((run / "summary.md").exists())

    def test_ref_crops_a_screenshot(self):
        shot = self.root / "shot.png"
        fake_emulator.render_gameplay(0).save(shot)
        out = self.root / "refs" / "radar.png"
        done = subprocess.run([sys.executable, str(HARNESS), "ref", str(shot), "--box", "0,0.62,0.22,1",
                               "--out", str(out)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        from PIL import Image
        with Image.open(out) as image:
            self.assertEqual(image.size, (round(0.22 * 320), 180 - round(0.62 * 180)))


if __name__ == "__main__":
    unittest.main()
