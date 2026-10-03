#!/usr/bin/env python3
"""Autonomous diagnosis loop driver for KytyPS5.

Launches kyty_emulator with the automation hooks (--automation-dir, --shader-capture-dir), plays a
scenario, watches the process, and classifies how the run ended:

    exit  result         meaning
    0     PASS           controllable in the prologue (and the optional soak finished cleanly)
    10    SHADER_ABORT   fatal error naming a shader hash; the capture is replayed and disassembled
    11    OTHER_ABORT    any other fatal exit, a signal, or an early exit
    12    HANG           the present count stopped moving while the process was alive
    13    STUCK          alive and rendering, but a scenario step (or the run) timed out
    2     (harness error: bad arguments, cannot launch, broken scenario)

Every run writes a directory under <build>/_Autoplay/<timestamp>/ that is also the emulator's
automation directory:

    cmd.txt stdout.txt _kyty.txt        how it was started and what it printed
    commands.txt events.jsonl           the command file the emulator tails, and its answers
    status.json status.jsonl latest.png latest heartbeat, heartbeat history, latest screenshot
    shots/ capture/                     named screenshots, shader captures
    meta.json exit.json progress.json   bookkeeping used by `classify`
    result.json summary.md              the verdict

See `kyty_autoplay.py --help`, tools/autoplay/README.md and .claude/skills/kyty-autoplay/SKILL.md.
Python 3.11+, standard library only; Pillow is needed for screenshot matching.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import tomllib
from pathlib import Path
from typing import Any, Callable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]

EXIT_PASS = 0
EXIT_HARNESS = 2
EXIT_SHADER_ABORT = 10
EXIT_OTHER_ABORT = 11
EXIT_HANG = 12
EXIT_STUCK = 13
RESULT_NAMES = {
    EXIT_HARNESS: "HARNESS_ERROR",
    EXIT_PASS: "PASS",
    EXIT_SHADER_ABORT: "SHADER_ABORT",
    EXIT_OTHER_ABORT: "OTHER_ABORT",
    EXIT_HANG: "HANG",
    EXIT_STUCK: "STUCK",
}

# The emulator ends a fatal error with DbgExit(321); the shell sees 321 & 0xff.
FATAL_EXIT_STATUS = 65
DEFAULT_HANG_SECONDS = 20.0
DEFAULT_BOOT_GRACE_SECONDS = 120.0
ACK_TIMEOUT_SECONDS = 10.0
SHOT_TIMEOUT_SECONDS = 20.0
STOP_GRACE_SECONDS = float(os.environ.get("KYTY_AUTOPLAY_STOP_GRACE", "10"))


# --------------------------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------------------------

class HarnessError(Exception):
    """Something is wrong with how the harness was used (not with the game)."""


def parse_duration(value: Any) -> float:
    """'5s', '250ms', '2m', '1h' or a bare number of seconds -> seconds."""
    if isinstance(value, (int, float)):
        return float(value)
    match = re.fullmatch(r"\s*([0-9]*\.?[0-9]+)\s*(ms|s|m|h)?\s*", str(value))
    if not match:
        raise HarnessError(f"bad duration {value!r} (use e.g. 5s, 250ms, 2m)")
    scale = {"ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0, None: 1.0}[match.group(2)]
    return float(match.group(1)) * scale


def read_json(path: Path, default: Any = None) -> Any:
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return default


def write_json(path: Path, data: Any) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, default=str) + "\n")
    tmp.replace(path)


def tail_lines(text: str, count: int) -> list[str]:
    return text.splitlines()[-count:]


def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def log(message: str) -> None:
    print(f"[autoplay] {message}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------------------------
# fatal error parsing and classification
# --------------------------------------------------------------------------------------------

FATAL_TITLES = ("--- Fatal Error ---", "--- Error ---")
# The emulator ends every fatal message with " in <file>:<line>" (some older reports used
# "(<file>:<line>)"). That suffix is also how the end of a block is recognised.
LOCATION_SUFFIX = re.compile(
    r"^(?P<text>.*?)(?:\s+in\s+|\s*\()(?P<file>\S+?\.(?:cpp|cc|cxx|c|h|hpp|inc|mm)):(?P<line>\d+)\)?\s*$")


@dataclasses.dataclass
class Fatal:
    """One fatal block as the emulator prints it (see Common::BuildFatalReport)."""
    title: str
    lines: list[str]
    build: str = ""
    file: str = ""
    line: int = 0

    @property
    def text(self) -> str:
        return "\n".join(self.lines)

    @property
    def first_line(self) -> str:
        return self.lines[0] if self.lines else ""


def find_fatals(text: str) -> list[Fatal]:
    """All fatal blocks in `text`, in order.

    A block is `--- Build ---`, the build label, `--- Error ---` or `--- Fatal Error ---`, then the
    message, whose last line ends in ` in <file>:<line>`. Whatever the emulator printed after that
    belongs to something else.
    """
    lines = text.splitlines()
    found: list[Fatal] = []
    for index, line in enumerate(lines):
        if line.strip() not in FATAL_TITLES:
            continue
        build = lines[index - 1] if index >= 2 and lines[index - 2].strip() == "--- Build ---" else ""
        body: list[str] = []
        location = None
        for follower in lines[index + 1:index + 41]:
            if follower.strip() == "--- Build ---":
                break
            body.append(follower)
            location = LOCATION_SUFFIX.match(follower)
            if location:
                break
        fatal = Fatal(title=line.strip(), lines=body, build=build)
        if location:
            body[-1] = location.group("text")
            fatal.file = Path(location.group("file")).name
            fatal.line = int(location.group("line"))
        elif body:
            # No location: keep what looks like the message and drop the log that followed it.
            end = next((i for i, text_line in enumerate(body) if not text_line.strip()), len(body))
            del body[end:]
        while body and not body[-1].strip():
            body.pop()
        found.append(fatal)
    return found


SHADER_HASH = re.compile(r"hash=0x([0-9a-fA-F]{1,16})")
SHADER_STAGE = re.compile(r"\bstage=([A-Za-z]+)")
SHADER_PC = re.compile(r"\bpc=0x([0-9a-fA-F]+)")


@dataclasses.dataclass
class ShaderFailure:
    hash: str          # 16 hex digits, lowercase, no 0x
    stage: str
    pc: Optional[int]
    reason: str        # the message after hash/stage/pc
    message: str       # the whole first line

    def to_json(self) -> dict:
        return {"hash": "0x" + self.hash, "stage": self.stage,
                "pc": None if self.pc is None else hex(self.pc),
                "reason": self.reason, "message": self.message}


def parse_shader_failure(fatal: Fatal) -> Optional[ShaderFailure]:
    """The shader named by a fatal error, or None when the error is not about a shader."""
    message = fatal.first_line
    match = SHADER_HASH.search(message)
    if not match:
        return None
    stage = SHADER_STAGE.search(message)
    pc = SHADER_PC.search(message)
    # The reason is whatever follows the last of hash=, stage=, pc= (they come in that order).
    reason = message[max(m.end() for m in (match, stage, pc) if m):].lstrip(": ").strip()
    return ShaderFailure(hash=match.group(1).lower().rjust(16, "0"),
                         stage=stage.group(1) if stage else "",
                         pc=int(pc.group(1), 16) if pc else None,
                         reason=reason, message=message)


@dataclasses.dataclass
class ExitInfo:
    """How the emulator process ended, as the supervisor saw it."""
    returncode: Optional[int] = None      # None while running, or when unknown
    signal: Optional[int] = None
    killed_for: str = ""                  # hang | timeout | stuck | done | stop | scenario_error | ""
    note: str = ""

    @classmethod
    def from_json(cls, data: dict) -> "ExitInfo":
        return cls(returncode=data.get("returncode"), signal=data.get("signal"),
                   killed_for=data.get("killed_for", ""), note=data.get("note", ""))


@dataclasses.dataclass
class Progress:
    """How far the scenario got."""
    steps_total: int = 0
    step_index: int = -1
    step_label: str = ""
    checkpoints: list[str] = dataclasses.field(default_factory=list)
    finished: bool = False                # every step (and the soak) ran to the end
    verified: bool = False                # a verify step proved the game is controllable
    passed: bool = False                  # finished and verified
    failure: str = ""                     # why a step gave up
    last_shot: str = ""

    @classmethod
    def from_json(cls, data: dict) -> "Progress":
        fields = {f.name for f in dataclasses.fields(cls)}
        return cls(**{k: v for k, v in (data or {}).items() if k in fields})


@dataclasses.dataclass
class Outcome:
    code: int
    reason: str
    fatal: Optional[Fatal] = None
    shader: Optional[ShaderFailure] = None
    label: str = ""                       # overrides the name for results that are not verdicts

    @property
    def name(self) -> str:
        return self.label or RESULT_NAMES.get(self.code, "UNKNOWN")


def usage_error(output: str) -> Optional[str]:
    """The message kyty_emulator printed before its usage text, or None if it did not print usage.

    PrintUsage prints the build string, then `kyty_emulator --game <dir|elf|zar> [options]`; a
    rejected argument is reported on the line above the build string.
    """
    lines = output.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("kyty_emulator --game <dir|elf|zar>"):
            before = [l for l in lines[max(0, index - 3):index - 1] if l.strip()]
            return before[-1].strip() if before else "(no message)"
    return None


def classify(exit_info: ExitInfo, output: str, progress: Progress) -> Outcome:
    """Decide how a run ended from the exit status, everything the emulator printed, and how far
    the scenario got. `output` is stdout plus the guest log; pure, so it can be tested on fixtures.
    """
    fatals = find_fatals(output)
    fatal = fatals[0] if fatals else None
    shader = parse_shader_failure(fatal) if fatal else None
    code = exit_info.returncode

    # The emulator rejected its command line: it prints why, then its usage, and exits 1.
    complaint = usage_error(output)
    if complaint is not None and code == 1 and not exit_info.killed_for:
        hint = ""
        if "unknown option" in complaint:
            hint = " (is kyty_emulator older than the harness? rebuild it)"
        return Outcome(EXIT_HARNESS, f"kyty_emulator rejected its command line: {complaint}{hint}")

    # Cases where the harness ended the run.
    if exit_info.killed_for == "hang":
        return Outcome(EXIT_HANG, exit_info.note or "present count stopped moving", fatal, shader)
    if exit_info.killed_for == "stop":
        # An interactive session ended by the user says nothing about the game.
        return Outcome(EXIT_PASS, "stopped by the user", fatal, shader, label="STOPPED")
    if exit_info.killed_for == "scenario_error":
        return Outcome(EXIT_HARNESS, exit_info.note or "scenario error")
    if exit_info.killed_for in ("timeout", "stuck"):
        return Outcome(EXIT_STUCK, progress.failure or exit_info.note or "timed out while still running",
                       fatal, shader)
    if exit_info.killed_for == "done":
        # The scenario ran to the end and the harness asked the emulator to quit.
        if progress.passed:
            note = "controllable in the prologue"
            if exit_info.signal is not None:
                note += " (the emulator ignored quit and was killed)"
            return Outcome(EXIT_PASS, note, fatal, shader)
        return Outcome(EXIT_STUCK, "the scenario finished without a verify step proving the game is "
                       "controllable", fatal, shader)

    # Otherwise the emulator ended by itself.
    unknown = code is None and exit_info.signal is None
    abnormal = exit_info.signal is not None or code not in (None, 0)
    if fatal is not None and (abnormal or unknown):
        if shader is not None:
            return Outcome(EXIT_SHADER_ABORT, shader.message, fatal, shader)
        return Outcome(EXIT_OTHER_ABORT, fatal.first_line or "fatal error", fatal, shader)
    if exit_info.signal is not None:
        known = {s.value: s.name for s in signal.Signals}
        return Outcome(EXIT_OTHER_ABORT, f"killed by signal {known.get(exit_info.signal, exit_info.signal)}",
                       fatal, shader)
    if abnormal:
        return Outcome(EXIT_OTHER_ABORT, f"exited with status {code}", fatal, shader)
    if code == 0:
        return Outcome(EXIT_OTHER_ABORT, "exited with status 0 before the scenario finished", fatal, shader)
    return Outcome(EXIT_OTHER_ABORT, "ended without a verdict", fatal, shader)


GUEST_FAULT_HINT = re.compile(
    r"(exception|fault|SIGSEGV|SIGILL|SIGBUS|SIGABRT|segmentation|unhandled|access violation|"
    r"backtrace|stack trace|not implemented|unimplemented)", re.IGNORECASE)


def guest_fault_context(output: str, context: int = 4, limit: int = 120) -> list[str]:
    """Lines that look like a guest crash report, with a few lines of context around each."""
    lines = output.splitlines()
    keep: set[int] = set()
    for index, line in enumerate(lines):
        if GUEST_FAULT_HINT.search(line):
            keep.update(range(max(0, index - context), min(len(lines), index + context + 1)))
    selected = sorted(keep)
    return [lines[i] for i in selected][-limit:]


# --------------------------------------------------------------------------------------------
# screenshots
# --------------------------------------------------------------------------------------------

def _pillow():
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        raise HarnessError("Pillow is required for screenshot matching (pip install pillow)")
    return Image


def crop_fraction(image, box: list[float]):
    """Crop `image` to a box given as fractions of its size: [x0, y0, x1, y1]."""
    if len(box) != 4 or not all(0.0 <= v <= 1.0 for v in box) or box[0] >= box[2] or box[1] >= box[3]:
        raise HarnessError(f"bad box {box!r}: need [x0, y0, x1, y1] fractions with x0<x1, y0<y1")
    width, height = image.size
    return image.crop((round(box[0] * width), round(box[1] * height),
                       max(round(box[2] * width), round(box[0] * width) + 1),
                       max(round(box[3] * height), round(box[1] * height) + 1)))


def gray_thumbnail(image, size: tuple[int, int]) -> list[int]:
    Image = _pillow()
    return list(image.convert("L").resize(size, Image.BOX).getdata())


def mean_abs_diff(a: list[int], b: list[int]) -> float:
    """Mean absolute difference of two equally sized gray thumbnails, 0 (same) to 1 (inverted)."""
    if len(a) != len(b) or not a:
        raise HarnessError("thumbnails differ in size")
    return sum(abs(x - y) for x, y in zip(a, b)) / (255.0 * len(a))


MATCH_SIZE = (32, 32)
FRAME_SIZE = (64, 64)


def match_crop(shot_path: Path, ref_path: Path, box: Optional[list[float]]) -> float:
    """Difference between the `box` region of a screenshot and a reference crop (0 = identical)."""
    Image = _pillow()
    with Image.open(shot_path) as shot, Image.open(ref_path) as ref:
        region = crop_fraction(shot, box) if box else shot
        return mean_abs_diff(gray_thumbnail(region, MATCH_SIZE), gray_thumbnail(ref, MATCH_SIZE))


def frame_diff(path_a: Path, path_b: Path) -> float:
    """How much two whole screenshots differ (0 = identical)."""
    Image = _pillow()
    with Image.open(path_a) as a, Image.open(path_b) as b:
        return mean_abs_diff(gray_thumbnail(a, FRAME_SIZE), gray_thumbnail(b, FRAME_SIZE))


# --------------------------------------------------------------------------------------------
# scenarios
# --------------------------------------------------------------------------------------------

STEP_ACTIONS = ("wait", "wait_frames", "press", "hold", "release", "send", "checkpoint", "until", "verify")


@dataclasses.dataclass
class Step:
    index: int
    action: str
    value: Any
    options: dict[str, Any]

    @property
    def label(self) -> str:
        return f"step {self.index + 1}: {self.action} {self.value if not isinstance(self.value, dict) else ''}".strip()


@dataclasses.dataclass
class Scenario:
    name: str
    path: Optional[Path]
    steps: list[Step]
    run: dict[str, Any]
    soak: dict[str, Any]
    game: str = ""


def resolve_ref(scenario_dir: Path, ref: str) -> Path:
    path = Path(ref)
    path = path if path.is_absolute() else scenario_dir / path
    if not path.is_file():
        raise HarnessError(f"reference image {ref!r} not found (looked at {path})")
    return path


def parse_while_waiting(spec: Any) -> list[tuple[str, float]]:
    """'press cross every 3s' (or a list of those) -> [(command, seconds)]."""
    specs = [spec] if isinstance(spec, str) else list(spec or [])
    actions = []
    for text in specs:
        match = re.fullmatch(r"\s*(.+?)\s+every\s+(\S+)\s*", str(text))
        if not match:
            raise HarnessError(f"bad while_waiting {text!r}: expected '<command> every <duration>'")
        actions.append((match.group(1), parse_duration(match.group(2))))
    return actions


def load_scenario(path: Optional[Path]) -> Scenario:
    if path is None:
        return Scenario(name="(none)", path=None, steps=[], run={}, soak={})
    try:
        data = tomllib.loads(path.read_text())
    except OSError as error:
        raise HarnessError(f"cannot read scenario {path}: {error}")
    except tomllib.TOMLDecodeError as error:
        raise HarnessError(f"scenario {path} is not valid TOML: {error}")
    steps = []
    for index, raw in enumerate(data.get("step", [])):
        actions = [key for key in STEP_ACTIONS if key in raw]
        if len(actions) != 1:
            raise HarnessError(f"{path} step {index + 1}: need exactly one of {', '.join(STEP_ACTIONS)} "
                               f"(found {actions or 'none'})")
        action = actions[0]
        options = {k: v for k, v in raw.items() if k != action}
        step = Step(index=index, action=action, value=raw[action], options=options)
        validate_step(step, path.parent)
        steps.append(step)
    return Scenario(name=str(data.get("name", path.stem)), path=path, steps=steps,
                    run=dict(data.get("run", {})), soak=dict(data.get("soak", {})),
                    game=str(data.get("game", "")))


def validate_step(step: Step, scenario_dir: Path) -> None:
    where = f"step {step.index + 1}"
    if step.action in ("wait",):
        parse_duration(step.value)
    elif step.action == "wait_frames":
        if not isinstance(step.value, int) or step.value <= 0:
            raise HarnessError(f"{where}: wait_frames needs a positive integer")
    elif step.action in ("press", "hold", "release", "send", "checkpoint"):
        if not isinstance(step.value, str) or not step.value.strip():
            raise HarnessError(f"{where}: {step.action} needs a non-empty string")
    elif step.action == "until":
        spec = step.value
        if not isinstance(spec, dict) or "ref" not in spec:
            raise HarnessError(f"{where}: until needs a table with at least ref")
        resolve_ref(scenario_dir, spec["ref"])
        if "box" in spec:
            crop_check = spec["box"]
            if len(crop_check) != 4:
                raise HarnessError(f"{where}: box needs four fractions")
        parse_duration(spec.get("timeout", "2m"))
        parse_while_waiting(spec.get("while_waiting"))
    elif step.action == "verify":
        if step.value != "controllable":
            raise HarnessError(f"{where}: the only verify is 'controllable'")
        radar = step.options.get("radar")
        if not isinstance(radar, dict) or "ref" not in radar:
            raise HarnessError(f"{where}: verify needs radar = {{ ref = ..., box = [...] }}")
        resolve_ref(scenario_dir, radar["ref"])
        parse_duration(step.options.get("idle", "3s"))
        parse_while_waiting(radar.get("while_waiting"))


# --------------------------------------------------------------------------------------------
# the emulator command line
# --------------------------------------------------------------------------------------------

# Options the harness owns. Anything the user's own arguments say about them is dropped.
HARNESS_OPTIONS = {"--printf-direction", "--printf-output-file", "--automation-dir",
                   "--automation-shot-interval", "--shader-capture-dir"}


def parse_run_script(path: Path) -> list[str]:
    """Arguments of the kyty_emulator invocation in a launcher-generated kyty_run.sh.

    Best effort: the first line mentioning kyty_emulator is split like a shell would. Returns the
    arguments after the executable; raises HarnessError if there is no such line.
    """
    for raw in read_text(path).splitlines():
        line = raw.strip()
        if "kyty_emulator" not in line or line.startswith("#"):
            continue
        try:
            words = shlex.split(line)
        except ValueError:
            continue
        for index, word in enumerate(words):
            if Path(word).name.startswith("kyty_emulator"):
                return words[index + 1:]
    raise HarnessError(f"no kyty_emulator command found in {path}")


def strip_options(args: list[str], names: set[str]) -> list[str]:
    """Remove `--name value` pairs (and `--name=value`) for the given option names."""
    result: list[str] = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        name = arg.split("=", 1)[0]
        if name in names:
            skip = "=" not in arg
            continue
        result.append(arg)
    return result


def build_command(emulator: Path, run_dir: Path, game_args: list[str], shot_interval: int) -> list[str]:
    args = strip_options(game_args, HARNESS_OPTIONS)
    return [str(emulator), *args,
            "--printf-direction", "File",
            "--printf-output-file", str(run_dir / "_kyty.txt"),
            "--automation-dir", str(run_dir),
            "--automation-shot-interval", str(shot_interval),
            "--shader-capture-dir", str(run_dir / "capture")]


# --------------------------------------------------------------------------------------------
# a running emulator
# --------------------------------------------------------------------------------------------

class RunEnded(Exception):
    """The run is over; `exit_info` says why."""
    def __init__(self, exit_info: ExitInfo):
        super().__init__(exit_info.killed_for or "process exited")
        self.exit_info = exit_info


class StepFailed(Exception):
    """A scenario step gave up while the game was still running."""


class ScenarioError(Exception):
    """The emulator rejected a command the scenario sent."""


class Session:
    """One emulator process, its automation directory, and the watchdog around them."""

    def __init__(self, run_dir: Path, command: list[str], *, cwd: Path,
                 hang_seconds: float = DEFAULT_HANG_SECONDS,
                 boot_grace: float = DEFAULT_BOOT_GRACE_SECONDS,
                 deadline: Optional[float] = None, env: Optional[dict[str, str]] = None):
        self.run_dir = run_dir
        self.command = command
        self.cwd = cwd
        self.hang_seconds = hang_seconds
        self.boot_grace = boot_grace
        self.deadline = deadline          # time.monotonic() value, or None
        self.env = env
        self.proc: Optional[subprocess.Popen] = None
        self.started = 0.0
        self.last_status: dict = {}
        self.status_log = run_dir / "status.jsonl"
        self._status_key: Any = None
        self._events_offset = 0
        self._events_buffer = b""
        self.events: list[dict] = []
        self._ack_cursor = 0
        self._command_lines = 0
        self._last_present_change = 0.0
        self._last_heartbeat_change = 0.0
        self._last_presents = 0
        self._last_uptime = -1
        self._saw_heartbeat = False
        self.shot_count = 0
        # Interactive sessions let a mistyped command through; scenarios treat it as an error.
        self.tolerate_nack = False

    # ---- process ----------------------------------------------------------------------------
    def launch(self) -> None:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        (self.run_dir / "shots").mkdir(exist_ok=True)
        (self.run_dir / "capture").mkdir(exist_ok=True)
        (self.run_dir / "cmd.txt").write_text(shlex.join(self.command) + "\n")
        output = open(self.run_dir / "stdout.txt", "wb")
        # Its own process group, so one signal reaches the emulator and anything it spawned.
        self.proc = subprocess.Popen(self.command, stdout=output, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, cwd=self.cwd, env=self.env,
                                     start_new_session=True)
        output.close()
        self.started = time.monotonic()
        self._last_present_change = self._last_heartbeat_change = self.started
        write_json(self.run_dir / "meta.json", {
            "command": self.command, "pid": self.proc.pid, "cwd": str(self.cwd),
            "started_unix": time.time(), "hang_seconds": self.hang_seconds,
            "boot_grace": self.boot_grace})

    @property
    def pid(self) -> int:
        assert self.proc is not None
        return self.proc.pid

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def exit_info(self, killed_for: str = "", note: str = "") -> ExitInfo:
        code = self.proc.returncode if self.proc else None
        return ExitInfo(returncode=None if code is None or code < 0 else code,
                        signal=-code if code is not None and code < 0 else None,
                        killed_for=killed_for, note=note)

    def kill_group(self, grace: float = 3.0) -> None:
        if self.proc is None or self.proc.poll() is not None:
            return
        for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, 5.0)):
            try:
                os.killpg(self.proc.pid, sig)
            except ProcessLookupError:
                break
            try:
                self.proc.wait(timeout=wait)
                break
            except subprocess.TimeoutExpired:
                continue

    def quit(self, grace: float = STOP_GRACE_SECONDS) -> None:
        """Ask the emulator to exit cleanly; kill it if it does not."""
        if not self.alive():
            return
        self.append_command("quit")
        try:
            self.proc.wait(timeout=grace)  # type: ignore[union-attr]
        except subprocess.TimeoutExpired:
            self.kill_group()

    # ---- watchdog ---------------------------------------------------------------------------
    def poll(self) -> None:
        """Refresh heartbeat and events. Raises RunEnded if the process ended or looks hung."""
        self._read_status()
        self._read_events()
        if self.proc is not None and self.proc.poll() is not None:
            raise RunEnded(self.exit_info())
        now = time.monotonic()
        limit = self.hang_seconds if self._last_presents > 0 else self.boot_grace
        if now - self._last_present_change > limit:
            raise RunEnded(self.exit_info("hang", f"present count flat at {self._last_presents} for "
                                          f"{now - self._last_present_change:.0f} s "
                                          f"({'after' if self._last_presents else 'before'} the first present)"))
        if self._saw_heartbeat and now - self._last_heartbeat_change > max(self.hang_seconds, 5.0):
            raise RunEnded(self.exit_info("hang", f"heartbeat stopped for "
                                          f"{now - self._last_heartbeat_change:.0f} s"))
        if self.deadline is not None and now > self.deadline:
            raise RunEnded(self.exit_info("timeout", "run timeout reached"))

    def _read_status(self) -> None:
        status = read_json(self.run_dir / "status.json")
        if not isinstance(status, dict):
            return
        now = time.monotonic()
        self._saw_heartbeat = True
        presents = int(status.get("host_presents", 0))
        uptime = int(status.get("uptime_ms", 0))
        if presents != self._last_presents:
            self._last_presents = presents
            self._last_present_change = now
        if uptime != self._last_uptime:
            self._last_uptime = uptime
            self._last_heartbeat_change = now
            entry = {"harness_t": round(now - self.started, 2), **status}
            with open(self.status_log, "a") as handle:
                handle.write(json.dumps(entry) + "\n")
        self.last_status = status

    def _read_events(self) -> None:
        path = self.run_dir / "events.jsonl"
        try:
            with open(path, "rb") as handle:
                handle.seek(self._events_offset)
                chunk = handle.read()
        except OSError:
            return
        self._events_offset += len(chunk)
        self._events_buffer += chunk
        *complete, self._events_buffer = self._events_buffer.split(b"\n")
        for raw in complete:
            try:
                event = json.loads(raw)
            except ValueError:
                continue
            if isinstance(event, dict):
                self.events.append(event)
        # A command the emulator refused means the scenario is wrong, not the game.
        while self._ack_cursor < len(self.events):
            event = self.events[self._ack_cursor]
            self._ack_cursor += 1
            if event.get("event") == "ack" and event.get("ok") is False and not self.tolerate_nack:
                raise ScenarioError(f"emulator rejected `{event.get('cmd')}`: {event.get('error')}")

    def sleep(self, seconds: float, interval: float = 0.2) -> None:
        """Sleep while watching the process. Raises RunEnded if it ends or hangs meanwhile."""
        end = time.monotonic() + seconds
        while True:
            self.poll()
            remaining = end - time.monotonic()
            if remaining <= 0:
                return
            time.sleep(min(interval, remaining))

    # ---- commands ---------------------------------------------------------------------------
    def append_command(self, command: str) -> int:
        """Append a command to commands.txt and return its line number."""
        path = self.run_dir / "commands.txt"
        if self._command_lines == 0 and path.exists():
            self._command_lines = len(read_text(path).splitlines())
        with open(path, "a") as handle:
            handle.write(command.strip() + "\n")
        self._command_lines += 1
        return self._command_lines

    def send(self, command: str, wait_ack: bool = False, timeout: float = ACK_TIMEOUT_SECONDS) -> int:
        start = len(self.events)
        line = self.append_command(command)
        if wait_ack:
            self.wait_event(lambda e: e.get("event") == "ack" and e.get("line") == line, timeout,
                            f"ack for `{command}`", start)
        return line

    def wait_event(self, match: Callable[[dict], bool], timeout: float, what: str,
                   start: int = 0) -> dict:
        """The first event after index `start` that `match` accepts."""
        end = time.monotonic() + timeout
        seen = start
        while True:
            self.poll()
            while seen < len(self.events):
                event = self.events[seen]
                seen += 1
                if match(event):
                    return event
            if time.monotonic() > end:
                raise TimeoutError(f"timed out waiting for {what}")
            time.sleep(0.05)

    def screenshot(self, name: str, timeout: float = SHOT_TIMEOUT_SECONDS) -> Path:
        """Ask for a screenshot of the next presented frame; returns the PNG path."""
        self.shot_count += 1
        self._read_events()
        start = len(self.events)          # only an answer to this request counts, not an old one
        self.send(f"shot {name}")
        event = self.wait_event(lambda e: e.get("event") == "shot" and e.get("name") == name,
                                timeout, f"screenshot {name}", start)
        if not event.get("ok"):
            raise TimeoutError(f"screenshot {name} failed: {event.get('error', 'unknown error')}")
        return Path(event["path"])


# --------------------------------------------------------------------------------------------
# running a scenario
# --------------------------------------------------------------------------------------------

DEFAULT_SOAK_SCRIPT = [
    "stick l 0 1 2500", "stick l 1 0.6 1500", "press cross 150", "stick l 0 -1 1500",
    "stick l -1 0.6 1500", "stick r 0.8 0 1200", "stick l 0 1 3000", "press circle 150",
]


class ScenarioRunner:
    def __init__(self, session: Session, scenario: Scenario, run_dir: Path):
        self.session = session
        self.scenario = scenario
        self.run_dir = run_dir
        self.progress = Progress(steps_total=len(scenario.steps))
        self.scenario_dir = scenario.path.parent if scenario.path else run_dir

    def save_progress(self) -> None:
        write_json(self.run_dir / "progress.json", dataclasses.asdict(self.progress))

    def run(self, soak_seconds: float = 0.0) -> None:
        for step in self.scenario.steps:
            self.progress.step_index = step.index
            self.progress.step_label = step.label
            self.save_progress()
            log(step.label)
            self.run_step(step)
        if soak_seconds > 0:
            self.progress.step_label = f"soak {soak_seconds:.0f}s"
            self.save_progress()
            self.soak(soak_seconds)
        self.progress.finished = True
        # Without a verify step nothing has shown the game is controllable, so there is no PASS.
        self.progress.passed = self.progress.verified
        self.progress.step_label = "done"
        self.save_progress()

    # ---- steps ------------------------------------------------------------------------------
    def run_step(self, step: Step) -> None:
        session = self.session
        if step.action == "wait":
            session.sleep(parse_duration(step.value))
        elif step.action == "wait_frames":
            self.wait_frames(int(step.value), parse_duration(step.options.get("timeout", "10m")))
        elif step.action == "press":
            ms = step.options.get("ms")
            session.send(f"press {step.value}" + (f" {int(ms)}" if ms else ""))
            session.sleep(float(ms or 120) / 1000.0 + 0.05)
        elif step.action in ("hold", "release", "send"):
            session.send(step.value if step.action == "send" else f"{step.action} {step.value}")
        elif step.action == "checkpoint":
            self.progress.checkpoints.append(str(step.value))
            self.save_progress()
            log(f"checkpoint {step.value}")
        elif step.action == "until":
            spec = step.value
            self.wait_for_ref(step, resolve_ref(self.scenario_dir, spec["ref"]), spec.get("box"),
                              float(spec.get("max_diff", 0.1)),
                              parse_duration(spec.get("timeout", "2m")),
                              parse_while_waiting(spec.get("while_waiting")))
        elif step.action == "verify":
            self.verify_controllable(step)

    def wait_frames(self, count: int, timeout: float) -> None:
        start = int(self.session.last_status.get("guest_flips", 0))
        end = time.monotonic() + timeout
        while int(self.session.last_status.get("guest_flips", 0)) - start < count:
            if time.monotonic() > end:
                self.fail(f"only {int(self.session.last_status.get('guest_flips', 0)) - start} of {count} "
                          f"frames in {timeout:.0f} s")
            self.session.sleep(0.25)

    def fail(self, reason: str) -> None:
        self.progress.failure = f"{self.progress.step_label}: {reason}"
        self.save_progress()
        raise StepFailed(self.progress.failure)

    def probe(self, label: str) -> Optional[Path]:
        """A screenshot kept as shots/<label>_last.png, or None if the game is not presenting."""
        try:
            source = self.session.screenshot(f"{label}_probe")
        except TimeoutError:
            return None
        target = self.run_dir / "shots" / f"{label}_last.png"
        try:
            shutil.copyfile(source, target)
        except OSError:
            return None
        self.progress.last_shot = str(target)
        return target

    def wait_for_ref(self, step: Step, ref: Path, box: Optional[list[float]], max_diff: float,
                     timeout: float, actions: list[tuple[str, float]], label: Optional[str] = None) -> Path:
        label = label or f"step{step.index + 1:02d}"
        end = time.monotonic() + timeout
        next_action = {command: time.monotonic() for command, _ in actions}
        best = 1.0
        while True:
            shot = self.probe(label)
            if shot is not None:
                diff = match_crop(shot, ref, box)
                best = min(best, diff)
                if diff <= max_diff:
                    keep = self.run_dir / "shots" / f"{label}_ok.png"
                    shutil.copyfile(shot, keep)
                    self.progress.last_shot = str(keep)
                    log(f"{step.label}: matched {ref.name} (diff {diff:.3f})")
                    return keep
            now = time.monotonic()
            if now > end:
                self.fail(f"{ref.name} not seen within {timeout:.0f} s (closest diff {best:.3f}, "
                          f"limit {max_diff:.3f})")
            for command, every in actions:
                if now >= next_action[command]:
                    self.session.send(command)
                    next_action[command] = now + every
            self.session.sleep(1.0)

    def verify_controllable(self, step: Step) -> None:
        opts = step.options
        radar = opts["radar"]
        # 1. The minimap only exists in free gameplay.
        self.wait_for_ref(step, resolve_ref(self.scenario_dir, radar["ref"]), radar.get("box"),
                          float(radar.get("max_diff", 0.12)), parse_duration(radar.get("timeout", "30s")),
                          parse_while_waiting(radar.get("while_waiting")), label="radar")
        # 2. Holding the stick moves the world more than standing still does.
        idle = parse_duration(opts.get("idle", "3s"))
        stick = str(opts.get("stick", "l 0 1 3000"))
        move_seconds = float(stick.split()[-1]) / 1000.0 if len(stick.split()) >= 4 else 3.0
        a = self.require_shot("idle_a")
        self.session.sleep(idle)
        b = self.require_shot("idle_b")
        baseline = frame_diff(a, b)
        self.session.send(f"stick {stick}" if not stick.startswith("stick") else stick)
        self.session.sleep(0.3)
        c = self.require_shot("move_a")
        self.session.sleep(max(move_seconds - 0.6, 0.5))
        d = self.require_shot("move_b")
        moved = frame_diff(c, d)
        needed = max(baseline * float(opts.get("min_gain", 2.0)), baseline + float(opts.get("min_delta", 0.01)))
        log(f"controllable check: idle diff {baseline:.4f}, moving diff {moved:.4f}, needed {needed:.4f}")
        if moved < needed:
            self.fail(f"the frame barely changed under stick input (idle {baseline:.4f}, moving "
                      f"{moved:.4f}, needed {needed:.4f}): not controllable")
        self.progress.verified = True
        self.save_progress()
        self.session.sleep(0.5)

    def require_shot(self, name: str) -> Path:
        shot = self.probe(name)
        if shot is None:
            self.fail(f"no screenshot for {name}: the game is not presenting frames")
        return shot  # type: ignore[return-value]

    def soak(self, seconds: float) -> None:
        script = list(self.scenario.soak.get("script", DEFAULT_SOAK_SCRIPT))
        end = time.monotonic() + seconds
        start_flips = int(self.session.last_status.get("guest_flips", 0))
        index = 0
        while time.monotonic() < end:
            command = script[index % len(script)]
            index += 1
            self.session.send(command)
            words = command.split()
            pause = float(words[-1]) / 1000.0 if words[0] in ("stick", "press") and words[-1].isdigit() else 1.0
            self.session.sleep(min(max(pause, 0.5) + 0.3, max(end - time.monotonic(), 0.1)))
        advanced = int(self.session.last_status.get("guest_flips", 0)) - start_flips
        if advanced <= 0:
            self.fail("the game did not flip a single frame during the soak")
        log(f"soak finished: {advanced} guest frames")


# --------------------------------------------------------------------------------------------
# post mortem
# --------------------------------------------------------------------------------------------

def find_captures(capture_root: Path, shader_hash: str) -> list[Path]:
    """Capture directories for a shader hash (16 hex digits), any stage."""
    if not capture_root.is_dir():
        return []
    return sorted(p for p in capture_root.iterdir()
                  if p.is_dir() and re.fullmatch(rf"[a-z]+_{shader_hash}_[0-9a-f]+", p.name))


def run_tool(command: list[str], timeout: float = 120.0) -> tuple[int, str]:
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return -1, f"could not run {shlex.join(command)}: {error}"
    return done.returncode, done.stdout + done.stderr


def replay_shader(emulator: Path, capture: Path, shader: ShaderFailure, out_dir: Path) -> dict:
    """Replay a capture offline and disassemble the failing instruction."""
    out_dir.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {"capture": str(capture)}
    code, text = run_tool([str(emulator), "--shader-replay", str(capture), "--out",
                           str(out_dir / "out.spv")])
    (out_dir / "replay.txt").write_text(text)
    result["replay_exit"] = code
    result["replay_ok"] = code == 0 and "REPLAY OK" in text
    fatals = find_fatals(text)
    replay_shader_failure = parse_shader_failure(fatals[0]) if fatals else None
    result["replay_message"] = (fatals[0].first_line if fatals else
                                next((l for l in text.splitlines() if l.startswith("REPLAY")), ""))
    # The replay reproduces the game when it dies with the same reason at the same pc.
    result["reproduces"] = bool(replay_shader_failure and replay_shader_failure.hash == shader.hash
                                and replay_shader_failure.pc == shader.pc
                                and replay_shader_failure.reason.split(" (op=")[0] == shader.reason.split(" (op=")[0])
    pc_args = ["--pc", hex(shader.pc), "--window", "20"] if shader.pc is not None else []
    code, text = run_tool([str(emulator), "--shader-disasm", str(capture), *pc_args])
    (out_dir / "disasm.txt").write_text(text)
    result["disasm_exit"] = code
    result["disasm_window"] = text.splitlines()[:80]
    return result


def capture_gdb(pid: int, out_path: Path, timeout: float = 90.0) -> str:
    """Thread backtraces of a live process, via gdb. Returns the text (also saved)."""
    if shutil.which("gdb") is None:
        text = "gdb is not installed; no backtraces\n"
    else:
        code, text = run_tool(["gdb", "-p", str(pid), "-batch", "-ex", "set pagination off",
                               "-ex", "info threads", "-ex", "thread apply all bt 40",
                               "-ex", "detach"], timeout=timeout)
        if code != 0 and "Thread" not in text:
            text += "\n(gdb could not attach; check kernel.yama.ptrace_scope)\n"
    out_path.write_text(text)
    return text


def last_status_entries(run_dir: Path, count: int = 5) -> list[dict]:
    entries = []
    for line in tail_lines(read_text(run_dir / "status.jsonl"), count):
        try:
            entries.append(json.loads(line))
        except ValueError:
            pass
    return entries


# Mirrors CommandBufferDebugOp in src/graphics/host_gpu/renderer/render.h; the heartbeat reports the
# raw number of the last operations recorded into each submitted command buffer.
DEBUG_OPS = ["DispatchDirect", "DrawIndex", "DrawIndexAuto", "EopWrite", "EopInterrupt", "EopWriteBack",
             "EopFlip", "EopWriteBackFlip", "EopOnlyFlip", "DispatchIndirect", "Unknown"]


def debug_op_name(op: Any) -> str:
    return DEBUG_OPS[op] if isinstance(op, int) and 0 <= op < len(DEBUG_OPS) else str(op)


def hang_report(status: dict) -> dict:
    """What the last heartbeat says about where a hung emulator was stuck."""
    ticks = [{"gpu_tick": s.get("gpu_tick"), "current_tick": s.get("current_tick"),
              "gpu_behind_by": (s.get("current_tick") or 0) - (s.get("gpu_tick") or 0)}
             for s in status.get("schedulers", [])]
    return {"host_presents": status.get("host_presents"), "guest_flips": status.get("guest_flips"),
            "pad_reads": status.get("pad_reads"), "last_shader": status.get("last_shader"),
            "schedulers": ticks,
            "recent_submits": [{**submit, "op_name": debug_op_name(submit.get("op"))}
                               for submit in status.get("recent_submits", [])[:4]]}


def build_result(run_dir: Path, outcome: Outcome, exit_info: ExitInfo, progress: Progress,
                 replay_emulator: Optional[Path], do_replay: bool = True) -> dict:
    meta = read_json(run_dir / "meta.json", {})
    result: dict[str, Any] = {
        "result": outcome.name, "code": outcome.code, "reason": outcome.reason,
        "run_dir": str(run_dir), "command": meta.get("command"),
        "returncode": exit_info.returncode, "signal": exit_info.signal,
        "killed_for": exit_info.killed_for,
        "checkpoints": progress.checkpoints,
        "furthest_checkpoint": progress.checkpoints[-1] if progress.checkpoints else None,
        "step": progress.step_label, "step_failure": progress.failure,
        "last_shot": progress.last_shot or (str(run_dir / "latest.png") if (run_dir / "latest.png").exists() else ""),
    }
    status = read_json(run_dir / "status.json", {})
    result["last_status"] = status
    if outcome.fatal is not None:
        result["fatal"] = {"title": outcome.fatal.title, "message": outcome.fatal.text,
                           "file": outcome.fatal.file, "line": outcome.fatal.line,
                           "build": outcome.fatal.build}
    if outcome.shader is not None:
        result["shader"] = outcome.shader.to_json()
        captures = find_captures(run_dir / "capture", outcome.shader.hash)
        result["shader"]["captures"] = [str(c) for c in captures]
        if captures and replay_emulator is not None and do_replay:
            result["shader"]["replay"] = replay_shader(replay_emulator, captures[0], outcome.shader,
                                                       run_dir / "replay")
        elif not captures:
            result["shader"]["replay"] = {"error": "no capture directory for this hash"}
    if outcome.code == EXIT_OTHER_ABORT:
        output = read_text(run_dir / "stdout.txt") + "\n" + read_text(run_dir / "_kyty.txt")
        result["guest_fault_context"] = guest_fault_context(output)
        result["log_tail"] = tail_lines(read_text(run_dir / "_kyty.txt"), 40)
    if outcome.code == EXIT_HANG:
        result["hang"] = hang_report(status)
        if (run_dir / "gdb.txt").exists():
            result["hang"]["gdb"] = "gdb.txt"
    return result


def write_summary(run_dir: Path, result: dict) -> None:
    lines = [f"# {result['result']} ({result['code']})", "", f"**Reason:** {result['reason']}", "",
             f"- run: `{result['run_dir']}`",
             f"- exit: returncode={result.get('returncode')} signal={result.get('signal')} "
             f"killed_for={result.get('killed_for') or '-'}",
             f"- scenario step: {result.get('step') or '-'}",
             f"- checkpoints: {', '.join(result.get('checkpoints') or []) or '-'}"]
    if result.get("step_failure"):
        lines.append(f"- step failure: {result['step_failure']}")
    if result.get("last_shot"):
        lines.append(f"- last screenshot: `{result['last_shot']}`")
    status = result.get("last_status") or {}
    if status:
        lines += ["", "## Last heartbeat", "",
                  f"uptime {status.get('uptime_ms', 0) / 1000:.0f} s, host presents "
                  f"{status.get('host_presents')}, guest flips {status.get('guest_flips')}, "
                  f"pad reads {status.get('pad_reads')}, GPU submits {status.get('submits')}"]
        shader = status.get("last_shader")
        if shader:
            lines.append(f"last shader into the compiler: {shader.get('stage')} {shader.get('hash')}")
    if "fatal" in result:
        fatal = result["fatal"]
        lines += ["", "## Fatal error", "", "```", fatal["message"] +
                  (f"  [{fatal['file']}:{fatal['line']}]" if fatal.get("file") else ""), "```"]
    shader = result.get("shader")
    if shader:
        lines += ["", "## Shader", "", f"- hash {shader['hash']}, stage {shader['stage'] or '?'}, "
                  f"pc {shader['pc'] or '?'}", f"- reason: {shader['reason']}",
                  f"- captures: {', '.join(shader.get('captures') or []) or 'none'}"]
        replay = shader.get("replay") or {}
        if "error" in replay:
            lines.append(f"- replay: {replay['error']}")
        elif replay:
            lines += [f"- replay: exit {replay.get('replay_exit')} — {replay.get('replay_message')}",
                      f"- replay reproduces the game failure: **{'yes' if replay.get('reproduces') else 'no'}**",
                      "", "### Disassembly around the failing pc (`=>` marks it)", "", "```"]
            lines += replay.get("disasm_window", [])
            lines.append("```")
    if result.get("guest_fault_context"):
        lines += ["", "## Guest fault context", "", "```"] + result["guest_fault_context"][-60:] + ["```"]
    if result.get("hang"):
        hang = result["hang"]
        lines += ["", "## Hang", "", f"- schedulers (gpu tick vs current tick): {hang.get('schedulers')}",
                  f"- recent submits: {hang.get('recent_submits')}",
                  f"- backtraces: `{run_dir / 'gdb.txt'}`" if hang.get("gdb") else "- backtraces: none"]
    if result.get("log_tail"):
        lines += ["", "## Log tail (_kyty.txt)", "", "```"] + result["log_tail"][-25:] + ["```"]
    (run_dir / "summary.md").write_text("\n".join(lines) + "\n")


def classify_run_dir(run_dir: Path, replay_emulator: Optional[Path], do_replay: bool = True) -> dict:
    """Rebuild result.json and summary.md for a run directory from the files in it."""
    exit_info = ExitInfo.from_json(read_json(run_dir / "exit.json", {}))
    progress = Progress.from_json(read_json(run_dir / "progress.json", {}))
    output = read_text(run_dir / "stdout.txt") + "\n" + read_text(run_dir / "_kyty.txt")
    outcome = classify(exit_info, output, progress)
    result = build_result(run_dir, outcome, exit_info, progress, replay_emulator, do_replay)
    write_json(run_dir / "result.json", result)
    write_summary(run_dir, result)
    return result


# --------------------------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------------------------

def default_build_dir() -> Path:
    return Path(os.environ.get("KYTY_BUILD_DIR", REPO_ROOT / "_Build" / "linux"))


class Config:
    """Everything the commands share, resolved from the command line."""

    def __init__(self, args: argparse.Namespace):
        self.build_dir = Path(args.build_dir) if args.build_dir else default_build_dir()
        self.emulator = Path(args.emulator) if args.emulator else self.build_dir / "kyty_emulator"
        self.replay_emulator = Path(args.replay_emulator) if getattr(args, "replay_emulator", None) else self.emulator
        self.root = Path(args.root) if args.root else self.build_dir / "_Autoplay"
        self.args = args

    def game_args(self, scenario: Optional[Scenario]) -> list[str]:
        args: list[str] = []
        run_script = Path(self.args.run_script) if self.args.run_script else self.build_dir / "kyty_run.sh"
        if run_script.is_file():
            try:
                args += parse_run_script(run_script)
            except HarnessError as error:
                if not (self.args.game or (scenario and scenario.game)):
                    raise
                log(f"ignoring {run_script}: {error}")
        game = self.args.game or (scenario.game if scenario else "")
        if game:
            args = strip_options(args, {"--game"}) + ["--game", game]
        args += list(self.args.arg or [])
        args += shlex.split(self.args.args or "")
        if "--game" not in args:
            raise HarnessError("no game given: pass --game <dir>, set game in the scenario, or put a "
                               "kyty_run.sh next to the emulator")
        return args

    def new_run_dir(self) -> Path:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        run_dir = self.root.resolve() / stamp
        suffix = 1
        while run_dir.exists():
            suffix += 1
            run_dir = self.root / f"{stamp}-{suffix}"
        run_dir.mkdir(parents=True)
        return run_dir

    def check_emulator(self) -> None:
        if not self.emulator.is_file():
            raise HarnessError(f"{self.emulator} does not exist; build it with "
                               f"`cmake --build {self.build_dir} --target kyty_emulator`")


def finish_session(session: Session, run_dir: Path, error: Optional[RunEnded],
                   progress: Progress, cfg: Config) -> dict:
    """Stop the emulator (saving evidence first when it hung) and write the verdict."""
    if error is None:
        # The scenario ran to the end: ask for a clean exit.
        session.quit()
        info = session.exit_info("done")
    else:
        info = error.exit_info
        if session.alive():
            if info.killed_for == "hang":
                log("hang detected; capturing backtraces")
                capture_gdb(session.pid, run_dir / "gdb.txt")
            session.quit(grace=3.0)       # kills the process group if it does not leave
        ended = session.exit_info()
        info = ExitInfo(returncode=ended.returncode, signal=ended.signal,
                        killed_for=info.killed_for, note=info.note)
    write_json(run_dir / "exit.json", dataclasses.asdict(info))
    return classify_run_dir(run_dir, cfg.replay_emulator)


def command_run(args: argparse.Namespace) -> int:
    cfg = Config(args)
    scenario = load_scenario(Path(args.scenario) if args.scenario else None)
    cfg.check_emulator()
    timeout = parse_duration(args.timeout if args.timeout is not None else scenario.run.get("timeout", "30m"))
    hang = parse_duration(args.hang_seconds if args.hang_seconds is not None
                          else scenario.run.get("hang_seconds", DEFAULT_HANG_SECONDS))
    grace = parse_duration(args.boot_grace if args.boot_grace is not None
                           else scenario.run.get("boot_grace", DEFAULT_BOOT_GRACE_SECONDS))
    soak = parse_duration(args.soak if args.soak is not None else scenario.soak.get("duration", 0))
    game_args = cfg.game_args(scenario)
    run_dir = cfg.new_run_dir()
    command = build_command(cfg.emulator, run_dir, game_args, args.shot_interval)
    session = Session(run_dir, command, cwd=Path(args.cwd) if args.cwd else cfg.build_dir,
                      hang_seconds=hang, boot_grace=grace,
                      deadline=time.monotonic() + timeout)
    session.launch()
    log(f"run {run_dir} (pid {session.pid}, scenario {scenario.name}, timeout {timeout:.0f}s)")
    runner = ScenarioRunner(session, scenario, run_dir)
    error: Optional[RunEnded] = None
    try:
        if scenario.steps or soak > 0:
            runner.run(soak)
        else:
            # No scenario: just watch the boot until something happens or the timeout hits.
            session.sleep(timeout + 1.0)
    except RunEnded as ended:
        error = ended
    except StepFailed as failed:
        error = RunEnded(session.exit_info("stuck", str(failed)))
    except ScenarioError as bad:
        error = RunEnded(session.exit_info("scenario_error", str(bad)))
    except BaseException:
        # Interrupted or crashed: never leave the emulator running behind us.
        session.kill_group()
        raise
    runner.save_progress()
    result = finish_session(session, run_dir, error, runner.progress, cfg)
    print(json.dumps({"result": result["result"], "code": result["code"], "reason": result["reason"],
                      "run_dir": str(run_dir)}, indent=2))
    log(f"{result['result']}: {result['reason']}")
    log(f"summary: {run_dir / 'summary.md'}")
    return result["code"]


def command_classify(args: argparse.Namespace) -> int:
    cfg = Config(args)
    run_dir = Path(args.run)
    if not run_dir.is_dir():
        raise HarnessError(f"{run_dir} is not a directory")
    result = classify_run_dir(run_dir, cfg.replay_emulator, do_replay=not args.no_replay)
    print(json.dumps({"result": result["result"], "code": result["code"], "reason": result["reason"]}, indent=2))
    return result["code"]


# ---- interactive mode ------------------------------------------------------------------------

def current_pointer(cfg: Config) -> Path:
    return cfg.root / "current.json"


def resolve_run_dir(cfg: Config, args: argparse.Namespace) -> Path:
    if getattr(args, "run", None):
        return Path(args.run)
    pointer = read_json(current_pointer(cfg))
    if not pointer or not Path(pointer.get("run_dir", "")).is_dir():
        raise HarnessError("no running session; start one with `kyty_autoplay.py start`")
    return Path(pointer["run_dir"])


# Button names people use for an Xbox-style pad, mapped to the DualSense names the emulator knows.
BUTTON_ALIASES = {
    "rb": "r1", "lb": "l1", "rt": "r2", "lt": "l2", "r1": "r1", "l1": "l1", "r2": "r2", "l2": "l2",
    "a": "cross", "b": "circle", "x": "square", "y": "triangle",
    "start": "options", "menu": "options", "select": "touchpad", "back": "touchpad", "view": "touchpad",
    "dpad_up": "up", "dpad_down": "down", "dpad_left": "left", "dpad_right": "right",
    "rs": "r3", "ls": "l3",
}
PAD_BUTTONS = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options",
               "touchpad", "up", "down", "left", "right"}


def normalize_buttons(text: str) -> str:
    """'RB', 'rb+a' or 'r1' -> the emulator's names ('r1', 'r1+cross'). Raises on unknown names."""
    names = []
    for part in re.split(r"[+,\s]+", text.strip().lower()):
        if not part:
            continue
        name = BUTTON_ALIASES.get(part, part)
        if name not in PAD_BUTTONS:
            raise HarnessError(f"unknown button {part!r}; use one of {', '.join(sorted(PAD_BUTTONS))} "
                               f"or {', '.join(sorted(k for k in BUTTON_ALIASES if k not in PAD_BUTTONS))}")
        names.append(name)
    if not names:
        raise HarnessError("no button given")
    return "+".join(names)


def normalize_command(text: str) -> str:
    """Resolve button aliases in press/hold/release commands ('press RB' -> 'press r1')."""
    words = text.split()
    if len(words) >= 2 and words[0] in ("press", "hold", "release") and words[1] != "all":
        try:
            words[1] = normalize_buttons(words[1])
        except HarnessError:
            pass            # let the emulator report it in its ack
    return " ".join(words)


def make_config(**values: Any) -> Config:
    """A Config without a command line (for the MCP server and tests)."""
    defaults = {"build_dir": None, "emulator": None, "replay_emulator": None, "root": None, "game": None,
                "run_script": None, "arg": None, "args": None}
    defaults.update(values)
    return Config(argparse.Namespace(**defaults))


def last_event_seq(run_dir: Path) -> int:
    seq = 0
    for line in read_text(run_dir / "events.jsonl").splitlines():
        try:
            seq = max(seq, int(json.loads(line).get("seq", 0)))
        except (ValueError, AttributeError, TypeError):
            continue
    return seq


def start_session(cfg: Config, *, scenario: Optional[Scenario] = None, cwd: Optional[str] = None,
                  hang_seconds: float = DEFAULT_HANG_SECONDS,
                  boot_grace: float = DEFAULT_BOOT_GRACE_SECONDS, shot_interval: int = 10,
                  wait: float = 60.0) -> dict:
    """Launch the emulator under a detached supervisor; returns as soon as it has a heartbeat."""
    cfg.check_emulator()
    game_args = cfg.game_args(scenario)
    run_dir = cfg.new_run_dir()
    command = build_command(cfg.emulator, run_dir, game_args, shot_interval)
    # A detached supervisor owns the emulator, so it can record how it exits after we are gone.
    supervisor = [sys.executable, str(Path(__file__).resolve()), "_supervise", "--run", str(run_dir),
                  "--cwd", str(cwd or cfg.build_dir), "--hang-seconds", str(hang_seconds),
                  "--boot-grace", str(boot_grace), "--emulator", str(cfg.emulator),
                  "--replay-emulator", str(cfg.replay_emulator), "--build-dir", str(cfg.build_dir)]
    write_json(run_dir / "supervisor.json", {"command": command})
    out = open(run_dir / "supervisor.log", "wb")
    subprocess.Popen(supervisor, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                     start_new_session=True)
    out.close()
    write_json(current_pointer(cfg), {"run_dir": str(run_dir)})
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        if (run_dir / "status.json").exists():
            return {"run_dir": str(run_dir), "status": "running"}
        if (run_dir / "exit.json").exists():
            break
        time.sleep(0.25)
    if (run_dir / "exit.json").exists():
        # The supervisor writes the verdict just after the exit record.
        for _ in range(100):
            if (run_dir / "result.json").exists():
                break
            time.sleep(0.1)
        result = read_json(run_dir / "result.json", {})
        return {"run_dir": str(run_dir), "status": "exited early", "result": result.get("result"),
                "reason": result.get("reason"), "code": result.get("code", EXIT_OTHER_ABORT) or EXIT_OTHER_ABORT}
    return {"run_dir": str(run_dir), "status": "started; no heartbeat yet (the game may still be loading)"}


def send_command(run_dir: Path, text: str, timeout: float = ACK_TIMEOUT_SECONDS) -> dict:
    """Append one automation command and return its ack (or {"ack": None, "error": ...})."""
    path = run_dir / "commands.txt"
    line = len(read_text(path).splitlines()) + 1
    with open(path, "a") as handle:
        handle.write(text.strip() + "\n")
    ack = wait_for_event(run_dir, lambda e: e.get("event") == "ack" and e.get("line") == line, timeout)
    if ack is None:
        return {"line": line, "cmd": text.strip(), "ok": False, "ack": None,
                "error": "no ack (is the game running and loaded?)"}
    return ack


def take_shot(run_dir: Path, name: str = "", timeout: float = SHOT_TIMEOUT_SECONDS) -> Path:
    """Screenshot of the next presented frame. Raises HarnessError if none arrives."""
    name = re.sub(r"[^A-Za-z0-9_.-]", "_", name or time.strftime("shot_%H%M%S"))
    after = last_event_seq(run_dir)       # an older shot with the same name does not count
    with open(run_dir / "commands.txt", "a") as handle:
        handle.write(f"shot {name}\n")
    event = wait_for_event(run_dir, lambda e: e.get("event") == "shot" and e.get("name") == name
                           and int(e.get("seq", 0)) > after, timeout)
    if event is None or not event.get("ok"):
        raise HarnessError(f"no screenshot: {event.get('error') if event else 'timed out (is the game presenting frames?)'}")
    return Path(event["path"])


def read_session_status(run_dir: Path) -> dict:
    status = read_json(run_dir / "status.json")
    exit_json = read_json(run_dir / "exit.json")
    meta = read_json(run_dir / "meta.json", {})
    report = {"run_dir": str(run_dir), "pid": meta.get("pid"), "running": exit_json is None,
              "status": status}
    if exit_json is not None:
        result = read_json(run_dir / "result.json", {})
        report["exit"] = exit_json
        report["result"] = result.get("result")
        report["reason"] = result.get("reason")
    return report


def stop_session(run_dir: Path, timeout: float = 60.0) -> dict:
    if (run_dir / "result.json").exists() and (run_dir / "exit.json").exists():
        result = read_json(run_dir / "result.json", {})
        return {"run_dir": str(run_dir), "result": result.get("result"), "reason": result.get("reason"),
                "note": "the game had already exited"}
    (run_dir / "stop.request").write_text("stop\n")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if (run_dir / "result.json").exists():
            result = read_json(run_dir / "result.json", {})
            return {"run_dir": str(run_dir), "result": result.get("result"), "reason": result.get("reason")}
        time.sleep(0.25)
    raise HarnessError(f"the supervisor of {run_dir} did not finish in {timeout:.0f} s")


def make_reference(shot: Path, out: Path, box: Optional[list[float]] = None,
                   pixels: Optional[list[int]] = None) -> dict:
    """Crop a screenshot into a reference image for an `until` or `verify` step."""
    Image = _pillow()
    with Image.open(shot) as image:
        if pixels:
            region = image.crop(tuple(int(v) for v in pixels))
        elif box:
            region = crop_fraction(image, [float(v) for v in box])
        else:
            raise HarnessError("give a box (fractions) or pixels")
        out.parent.mkdir(parents=True, exist_ok=True)
        region.save(out)
        return {"path": str(out), "size": list(region.size), "source": str(shot),
                "source_size": list(image.size)}


def command_start(args: argparse.Namespace) -> int:
    cfg = Config(args)
    scenario = load_scenario(Path(args.scenario) if args.scenario else None)
    started = start_session(cfg, scenario=scenario, cwd=args.cwd, hang_seconds=args.hang_seconds,
                            boot_grace=args.boot_grace, shot_interval=args.shot_interval, wait=args.wait)
    code = started.pop("code", 0)
    print(json.dumps(started))
    return code


def command_supervise(args: argparse.Namespace) -> int:
    """Hidden: babysit one interactive emulator until it ends or a stop is requested."""
    cfg = Config(args)
    run_dir = Path(args.run)
    command = read_json(run_dir / "supervisor.json")["command"]
    session = Session(run_dir, command, cwd=Path(args.cwd), hang_seconds=args.hang_seconds,
                      boot_grace=args.boot_grace)
    session.tolerate_nack = True
    session.launch()
    error: Optional[RunEnded] = None
    try:
        while True:
            session.poll()
            if (run_dir / "stop.request").exists():
                error = RunEnded(session.exit_info("stop", "stopped by the user"))
                break
            time.sleep(0.25)
    except RunEnded as ended:
        error = ended
    exit_info = error.exit_info if error else session.exit_info()
    if exit_info.killed_for == "hang":
        capture_gdb(session.pid, run_dir / "gdb.txt")
    if session.alive():
        session.quit(grace=5.0)
    if session.proc is not None and session.proc.poll() is None:
        session.kill_group()
    final = session.exit_info(exit_info.killed_for, exit_info.note)
    write_json(run_dir / "exit.json", dataclasses.asdict(final))
    classify_run_dir(run_dir, cfg.replay_emulator)
    return 0


def command_send(args: argparse.Namespace) -> int:
    cfg = Config(args)
    ack = send_command(resolve_run_dir(cfg, args), normalize_command(args.text), args.timeout)
    print(json.dumps(ack))
    return 0 if ack.get("ok") else EXIT_HARNESS


def wait_for_event(run_dir: Path, match: Callable[[dict], bool], timeout: float) -> Optional[dict]:
    end = time.monotonic() + timeout
    while True:
        for line in read_text(run_dir / "events.jsonl").splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if isinstance(event, dict) and match(event):
                return event
        if time.monotonic() > end:
            return None
        if (run_dir / "exit.json").exists():
            return None
        time.sleep(0.1)


def command_shot(args: argparse.Namespace) -> int:
    cfg = Config(args)
    print(take_shot(resolve_run_dir(cfg, args), args.name or "", args.timeout))
    return 0


def command_status(args: argparse.Namespace) -> int:
    cfg = Config(args)
    print(json.dumps(read_session_status(resolve_run_dir(cfg, args)), indent=2))
    return 0


def command_stop(args: argparse.Namespace) -> int:
    cfg = Config(args)
    print(json.dumps(stop_session(resolve_run_dir(cfg, args), args.timeout)))
    return 0


def command_ref(args: argparse.Namespace) -> int:
    pixels = [int(v) for v in args.pixels.split(",")] if args.pixels else None
    box = [float(v) for v in args.box.split(",")] if args.box else None
    made = make_reference(Path(args.shot), Path(args.out), box=box, pixels=pixels)
    print(f"{made['path']}: {made['size'][0]}x{made['size'][1]} crop of {Path(made['source']).name} "
          f"({made['source_size'][0]}x{made['source_size'][1]})")
    return 0


# --------------------------------------------------------------------------------------------
# argument parsing
# --------------------------------------------------------------------------------------------

def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--build-dir", help="CMake build directory (default: _Build/linux or $KYTY_BUILD_DIR)")
    parser.add_argument("--emulator", help="path to kyty_emulator (default: <build-dir>/kyty_emulator)")
    parser.add_argument("--replay-emulator", help="kyty_emulator used for --shader-replay/--shader-disasm "
                        "(default: the same binary)")
    parser.add_argument("--root", help="where run directories go (default: <build-dir>/_Autoplay)")


def add_launch(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--game", help="game directory (default: from the scenario or kyty_run.sh)")
    parser.add_argument("--run-script", help="launcher script to take emulator flags from "
                        "(default: <build-dir>/kyty_run.sh when present)")
    parser.add_argument("--arg", action="append",
                        help="one extra kyty_emulator argument; repeatable (write --arg=--gpu for a "
                        "value that starts with dashes)")
    parser.add_argument("--args", help="extra kyty_emulator arguments as one quoted string, "
                        "e.g. --args \"--vulkan-validation true --gpu 1\"")
    parser.add_argument("--cwd", help="working directory for the emulator (default: the build dir)")
    parser.add_argument("--scenario", help="scenario TOML file")
    parser.add_argument("--shot-interval", type=int, default=10,
                        help="seconds between automatic screenshots refreshing latest.png (default 10)")
    parser.add_argument("--hang-seconds", type=float, default=DEFAULT_HANG_SECONDS,
                        help="seconds the present count may stay flat after the first present")
    parser.add_argument("--boot-grace", type=float, default=DEFAULT_BOOT_GRACE_SECONDS,
                        help="seconds allowed before the first present")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="play a scenario and classify the result")
    add_common(run)
    add_launch(run)
    run.set_defaults(hang_seconds=None, boot_grace=None, timeout=None, soak=None)
    run.add_argument("--timeout", help="give up after this long, e.g. 30m (default: scenario or 30m)")
    run.add_argument("--soak", help="after the scenario, keep moving for this long, e.g. 300 or 5m")
    run.set_defaults(func=command_run)

    start = sub.add_parser("start", help="interactive: launch the emulator in the background")
    add_common(start)
    add_launch(start)
    start.add_argument("--wait", type=float, default=60.0, help="seconds to wait for the first heartbeat")
    start.set_defaults(func=command_start)

    sup = sub.add_parser("_supervise")
    add_common(sup)
    sup.add_argument("--run", required=True)
    sup.add_argument("--cwd", required=True)
    sup.add_argument("--hang-seconds", type=float, default=DEFAULT_HANG_SECONDS)
    sup.add_argument("--boot-grace", type=float, default=DEFAULT_BOOT_GRACE_SECONDS)
    sup.set_defaults(func=command_supervise, game=None, run_script=None, arg=None, args=None)

    send = sub.add_parser("send", help="interactive: send one command, e.g. 'press cross'")
    add_common(send)
    send.add_argument("text")
    send.add_argument("--run", help="run directory (default: the current session)")
    send.add_argument("--timeout", type=float, default=ACK_TIMEOUT_SECONDS)
    send.set_defaults(func=command_send, game=None, run_script=None)

    shot = sub.add_parser("shot", help="interactive: save a screenshot and print its path")
    add_common(shot)
    shot.add_argument("name", nargs="?")
    shot.add_argument("--run")
    shot.add_argument("--timeout", type=float, default=SHOT_TIMEOUT_SECONDS)
    shot.set_defaults(func=command_shot, game=None, run_script=None)

    status = sub.add_parser("status", help="interactive: print the heartbeat")
    add_common(status)
    status.add_argument("--run")
    status.set_defaults(func=command_status, game=None, run_script=None)

    stop = sub.add_parser("stop", help="interactive: quit the emulator and write the result")
    add_common(stop)
    stop.add_argument("--run")
    stop.add_argument("--timeout", type=float, default=60.0)
    stop.set_defaults(func=command_stop, game=None, run_script=None)

    cls = sub.add_parser("classify", help="rebuild result.json for an existing run directory")
    add_common(cls)
    cls.add_argument("run", help="run directory")
    cls.add_argument("--no-replay", action="store_true", help="do not re-run the shader replay")
    cls.set_defaults(func=command_classify, game=None, run_script=None)

    ref = sub.add_parser("ref", help="crop a screenshot into a reference image for a scenario")
    ref.add_argument("shot")
    group = ref.add_mutually_exclusive_group(required=True)
    group.add_argument("--box", help="x0,y0,x1,y1 as fractions of the image, e.g. 0.02,0.7,0.2,0.98")
    group.add_argument("--pixels", help="x0,y0,x1,y1 in pixels")
    ref.add_argument("--out", required=True)
    ref.set_defaults(func=command_ref)
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except HarnessError as error:
        print(f"kyty_autoplay: {error}", file=sys.stderr)
        return EXIT_HARNESS
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
