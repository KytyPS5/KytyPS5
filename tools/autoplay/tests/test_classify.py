"""Classification of how a run ended: fatal error parsing, shader failures, exit statuses."""
import json
import signal
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import kyty_autoplay as ka  # noqa: E402

FIXTURES = HERE / "fixtures"


def fixture(name: str) -> str:
    return (FIXTURES / name).read_text()


def passed_progress() -> ka.Progress:
    return ka.Progress(steps_total=3, step_index=2, checkpoints=["menu", "gameplay"], finished=True,
                       verified=True, passed=True)


class FatalParsing(unittest.TestCase):
    def test_block_and_location(self):
        fatals = ka.find_fatals(fixture("shader_abort.stdout.txt"))
        self.assertEqual(len(fatals), 1)
        fatal = fatals[0]
        self.assertEqual(fatal.title, "--- Error ---")
        self.assertEqual(fatal.build, "Source build e317465-dirty")
        self.assertEqual(fatal.file, "ResourceTracking.cpp")
        self.assertEqual(fatal.line, 367)
        self.assertTrue(fatal.first_line.startswith("shader resource tracking: hash=0x6a53456e7ef5d1b0"))
        self.assertNotIn("ResourceTracking.cpp", fatal.first_line)

    def test_duplicate_blocks_from_stdout_and_log(self):
        text = fixture("shader_abort.stdout.txt") * 2
        self.assertEqual(len(ka.find_fatals(text)), 2)

    def test_location_on_its_own_line(self):
        fatal = ka.find_fatals(fixture("spirv_validation_abort.txt"))[0]
        self.assertEqual(fatal.first_line, "ShaderRecompiler PS failed hash=0x00000000deadbeef: SPIR-V validation failed")
        self.assertEqual((fatal.file, fatal.line), ("pipelineCache.cpp", 252))

    def test_fatal_error_title(self):
        fatal = ka.find_fatals(fixture("other_abort.stdout.txt"))[0]
        self.assertEqual(fatal.title, "--- Fatal Error ---")
        self.assertEqual(fatal.first_line, "Not implemented (result != vk::Result::eSuccess)")
        self.assertEqual(fatal.file, "masterSemaphore.cpp")

    def test_block_ends_at_its_location_even_when_more_log_follows(self):
        text = (fixture("shader_abort.stdout.txt") + "fake emulator starting\nREPLAY capture=/x\n")
        fatal = ka.find_fatals(text)[0]
        self.assertEqual(len(fatal.lines), 1)
        self.assertNotIn("ResourceTracking.cpp", fatal.first_line)
        self.assertEqual(ka.parse_shader_failure(fatal).reason.split(" (op=")[0],
                         "buffer descriptor is not a valid runtime value; GPU-selected access requires "
                         "a raw DWORD x2/x3/x4 load")

    def test_multi_line_message(self):
        text = ("--- Build ---\nlabel\n--- Error ---\nfirst line\nsecond line in /a/b/thing.cpp:9\ntrailing log\n")
        fatal = ka.find_fatals(text)[0]
        self.assertEqual(fatal.lines, ["first line", "second line"])
        self.assertEqual((fatal.file, fatal.line), ("thing.cpp", 9))

    def test_message_without_a_location_stops_at_the_blank_line(self):
        text = "--- Build ---\nlabel\n--- Error ---\njust a message\n\nunrelated log\n"
        self.assertEqual(ka.find_fatals(text)[0].lines, ["just a message"])

    def test_no_fatal(self):
        self.assertEqual(ka.find_fatals(fixture("clean_exit.stdout.txt")), [])


class ShaderFailureParsing(unittest.TestCase):
    def failure(self, name):
        return ka.parse_shader_failure(ka.find_fatals(fixture(name))[0])

    def test_gta_resource_tracking_failure(self):
        shader = self.failure("shader_abort.stdout.txt")
        self.assertEqual(shader.hash, "6a53456e7ef5d1b0")
        self.assertEqual(shader.stage, "compute")
        self.assertEqual(shader.pc, 0x86c)
        self.assertTrue(shader.reason.startswith(
            "buffer descriptor is not a valid runtime value; GPU-selected access requires a raw "
            "DWORD x2/x3/x4 load"))
        self.assertIn("op=StoreBufferU32", shader.reason)
        self.assertEqual(shader.to_json()["pc"], "0x86c")
        self.assertEqual(shader.to_json()["hash"], "0x6a53456e7ef5d1b0")

    def test_message_as_first_reported(self):
        shader = self.failure("shader_abort_old_message.txt")
        self.assertEqual((shader.hash, shader.stage, shader.pc), ("6a53456e7ef5d1b0", "compute", 0x86c))
        self.assertTrue(shader.reason.startswith("buffer descriptor is not a valid runtime value"))

    def test_failure_without_stage_or_pc(self):
        shader = self.failure("spirv_validation_abort.txt")
        self.assertEqual(shader.hash, "00000000deadbeef")
        self.assertEqual(shader.stage, "")
        self.assertIsNone(shader.pc)
        self.assertEqual(shader.reason, "SPIR-V validation failed")

    def test_short_hash_is_padded(self):
        fatal = ka.Fatal("--- Error ---", ["thing failed hash=0xb2: boom"])
        self.assertEqual(ka.parse_shader_failure(fatal).hash, "00000000000000b2")

    def test_non_shader_fatal(self):
        self.assertIsNone(ka.parse_shader_failure(ka.find_fatals(fixture("other_abort.stdout.txt"))[0]))


class Classification(unittest.TestCase):
    def test_shader_abort(self):
        outcome = ka.classify(ka.ExitInfo(returncode=65), fixture("shader_abort.stdout.txt"), ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_SHADER_ABORT)
        self.assertEqual(outcome.name, "SHADER_ABORT")
        self.assertEqual(outcome.shader.hash, "6a53456e7ef5d1b0")
        self.assertIn("GPU-selected access", outcome.reason)

    def test_shader_abort_when_fatal_is_only_in_the_log_file(self):
        outcome = ka.classify(ka.ExitInfo(returncode=65), "nothing useful here\n" + fixture("shader_abort.stdout.txt"),
                              ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_SHADER_ABORT)

    def test_other_abort_from_non_shader_fatal(self):
        outcome = ka.classify(ka.ExitInfo(returncode=65), fixture("other_abort.stdout.txt"), ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_OTHER_ABORT)
        self.assertIsNone(outcome.shader)
        self.assertIn("Not implemented", outcome.reason)

    def test_other_abort_from_signal(self):
        outcome = ka.classify(ka.ExitInfo(signal=signal.SIGSEGV), fixture("guest_fault.log"), ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_OTHER_ABORT)
        self.assertIn("SIGSEGV", outcome.reason)

    def test_other_abort_from_unexpected_status(self):
        outcome = ka.classify(ka.ExitInfo(returncode=66), "", ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_OTHER_ABORT)
        self.assertIn("66", outcome.reason)

    def test_early_clean_exit_is_not_a_pass(self):
        outcome = ka.classify(ka.ExitInfo(returncode=0), fixture("clean_exit.stdout.txt"),
                              ka.Progress(steps_total=3, step_index=1))
        self.assertEqual(outcome.code, ka.EXIT_OTHER_ABORT)
        self.assertIn("before the scenario finished", outcome.reason)

    def test_fatal_text_with_clean_exit_is_ignored(self):
        outcome = ka.classify(ka.ExitInfo(returncode=0, killed_for="done"), fixture("shader_abort.stdout.txt"),
                              passed_progress())
        self.assertEqual(outcome.code, ka.EXIT_PASS)

    def test_hang(self):
        outcome = ka.classify(ka.ExitInfo(signal=signal.SIGKILL, killed_for="hang", note="present count flat"),
                              "", ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_HANG)
        self.assertEqual(outcome.reason, "present count flat")

    def test_timeout_and_stuck_step(self):
        stuck = ka.classify(ka.ExitInfo(signal=signal.SIGKILL, killed_for="stuck"), "",
                            ka.Progress(failure="step 4: radar.png not seen within 60 s"))
        self.assertEqual(stuck.code, ka.EXIT_STUCK)
        self.assertIn("radar.png", stuck.reason)
        self.assertEqual(ka.classify(ka.ExitInfo(killed_for="timeout"), "", ka.Progress()).code, ka.EXIT_STUCK)

    def test_pass_needs_a_verified_scenario(self):
        done = ka.ExitInfo(returncode=0, killed_for="done")
        self.assertEqual(ka.classify(done, "", passed_progress()).code, ka.EXIT_PASS)
        unverified = ka.Progress(steps_total=2, finished=True, verified=False, passed=False)
        outcome = ka.classify(done, "", unverified)
        self.assertEqual(outcome.code, ka.EXIT_STUCK)
        self.assertIn("verify", outcome.reason)

    def test_pass_even_if_the_emulator_had_to_be_killed_after_quit(self):
        outcome = ka.classify(ka.ExitInfo(signal=signal.SIGKILL, killed_for="done"), "", passed_progress())
        self.assertEqual(outcome.code, ka.EXIT_PASS)
        self.assertIn("killed", outcome.reason)

    def test_abort_while_soaking_is_an_abort(self):
        progress = ka.Progress(steps_total=3, verified=True, finished=False, passed=False)
        outcome = ka.classify(ka.ExitInfo(returncode=65), fixture("other_abort.stdout.txt"), progress)
        self.assertEqual(outcome.code, ka.EXIT_OTHER_ABORT)

    def test_scenario_error_is_a_harness_error(self):
        outcome = ka.classify(ka.ExitInfo(killed_for="scenario_error", note="rejected `dance`"), "", ka.Progress())
        self.assertEqual(outcome.code, ka.EXIT_HARNESS)

    def test_exit_codes_are_the_documented_ones(self):
        self.assertEqual((ka.EXIT_PASS, ka.EXIT_SHADER_ABORT, ka.EXIT_OTHER_ABORT, ka.EXIT_HANG, ka.EXIT_STUCK),
                         (0, 10, 11, 12, 13))


class HangReport(unittest.TestCase):
    def test_names_ops_and_computes_how_far_the_gpu_is_behind(self):
        report = ka.hang_report({"host_presents": 5, "guest_flips": 4, "pad_reads": 9,
                                 "schedulers": [{"gpu_tick": 100, "current_tick": 130}],
                                 "recent_submits": [{"tick": 130, "op": 0}, {"tick": 129, "op": 99}]})
        self.assertEqual(report["schedulers"][0]["gpu_behind_by"], 30)
        self.assertEqual([s["op_name"] for s in report["recent_submits"]], ["DispatchDirect", "99"])

    def test_empty_heartbeat(self):
        self.assertEqual(ka.hang_report({})["schedulers"], [])


class GuestFaultContext(unittest.TestCase):
    def test_extracts_the_crash_report(self):
        lines = ka.guest_fault_context(fixture("guest_fault.log"))
        joined = "\n".join(lines)
        self.assertIn("Unhandled exception 0xc0000005", joined)
        self.assertIn("rip=0x0000000800a1b2c3", joined)

    def test_quiet_log_has_no_context(self):
        self.assertEqual(ka.guest_fault_context("all fine\nstill fine\n"), [])


class RunDirectory(unittest.TestCase):
    def make_run(self, root: Path, stdout: str, exit_info: dict, progress: dict = None) -> Path:
        run = root / "run"
        run.mkdir()
        (run / "stdout.txt").write_text(stdout)
        (run / "_kyty.txt").write_text("")
        (run / "exit.json").write_text(json.dumps(exit_info))
        (run / "progress.json").write_text(json.dumps(progress or {}))
        (run / "meta.json").write_text(json.dumps({"command": ["kyty_emulator", "--game", "x"]}))
        return run

    def test_classify_rebuilds_the_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.make_run(Path(tmp), fixture("shader_abort.stdout.txt"), {"returncode": 65})
            result = ka.classify_run_dir(run, None)
            self.assertEqual(result["result"], "SHADER_ABORT")
            self.assertEqual(result["shader"]["hash"], "0x6a53456e7ef5d1b0")
            self.assertEqual(result["shader"]["pc"], "0x86c")
            self.assertEqual(result["shader"]["replay"], {"error": "no capture directory for this hash"})
            self.assertEqual(json.loads((run / "result.json").read_text())["code"], ka.EXIT_SHADER_ABORT)
            summary = (run / "summary.md").read_text()
            self.assertIn("SHADER_ABORT", summary)
            self.assertIn("0x6a53456e7ef5d1b0", summary)
            self.assertIn("GPU-selected access", summary)

    def test_other_abort_keeps_the_fault_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.make_run(Path(tmp), fixture("other_abort.stdout.txt"), {"returncode": 65})
            (run / "_kyty.txt").write_text(fixture("guest_fault.log"))
            result = ka.classify_run_dir(run, None)
            self.assertEqual(result["result"], "OTHER_ABORT")
            self.assertIn("Not implemented", result["fatal"]["message"])
            self.assertTrue(any("access violation" in line for line in result["guest_fault_context"]))

    def test_find_captures_matches_any_stage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("cs_6a53456e7ef5d1b0_0badf00d", "cs_6a53456e7ef5d1b0_00000001", "ps_1111111111111111_00000000",
                         "cs_6a53456e7ef5d1b0.txt"):
                (root / name).mkdir()
            found = [p.name for p in ka.find_captures(root, "6a53456e7ef5d1b0")]
            self.assertEqual(found, ["cs_6a53456e7ef5d1b0_00000001", "cs_6a53456e7ef5d1b0_0badf00d"])


if __name__ == "__main__":
    unittest.main()
