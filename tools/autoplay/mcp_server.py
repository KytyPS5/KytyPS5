#!/usr/bin/env python3
"""MCP server that lets an agent (Claude Code) play the game in KytyPS5.

It drives the emulator through the automation hooks (the same interactive session as
`kyty_autoplay.py start/send/shot/stop`) and can ask a local Ollama vision model what is on screen.
Register it with the `.mcp.json` at the repository root, or:

    claude mcp add kyty -- python3 tools/autoplay/mcp_server.py

Environment:
    KYTY_BUILD_DIR       build directory with kyty_emulator (default _Build/linux)
    KYTY_EMULATOR        path to kyty_emulator (default <build>/kyty_emulator)
    KYTY_GAME            game directory, used when start_game gets none (or use kyty_run.sh)
    KYTY_EMULATOR_ARGS   extra kyty_emulator arguments, e.g. "--gpu 0"
    KYTY_BOOT_GRACE      seconds allowed before the first frame (default 300)
    OLLAMA_HOST, KYTY_VISION_MODEL, KYTY_VISION_MAX_WIDTH, KYTY_VISION_TIMEOUT: see vision.py

Needs the `mcp` package (pip install -r tools/autoplay/requirements-mcp.txt); 1.x and 2.x work.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path
from typing import Any, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kyty_autoplay as ka  # noqa: E402
import vision  # noqa: E402

REFS_DIR = Path(__file__).resolve().parent / "scenarios" / "refs"
MAX_WAIT_SECONDS = 120.0
MAX_PRESS_TIMES = 20

INSTRUCTIONS = """Plays a PS5 game (GTA V) inside the KytyPS5 emulator.

Loop: start_game, then look (asks a local vision model to describe the screen) or screenshot (see it
yourself), decide, press/stick, wait, and look again. Always stop_game when done.

Buttons: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad up down left right. Xbox
names work too: a=cross b=circle x=square y=triangle lb=l1 rb=r1 lt=l2 rt=r2 start=options.
Sticks: x,y in -1..1, y=+1 is forward/up. The vision model can be wrong; check with screenshot when
an answer looks surprising. When a sequence works, save_reference a crop that identifies the screen so
it can become a replayable step in tools/autoplay/scenarios/gta5_story.toml."""


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return default


class GameTools:
    """The tools, independent of the MCP SDK so they can be tested directly."""

    def __init__(self, cfg: Optional[ka.Config] = None):
        self.cfg = cfg or ka.make_config(build_dir=os.environ.get("KYTY_BUILD_DIR"),
                                         emulator=os.environ.get("KYTY_EMULATOR"))
        self.run_dir: Optional[Path] = None
        self.last_shot: Optional[Path] = None
        self.shot_count = 0

    # ---- helpers ------------------------------------------------------------------------------
    def _session(self) -> Path:
        if self.run_dir is not None:
            return self.run_dir
        try:
            self.run_dir = ka.resolve_run_dir(self.cfg, argparse.Namespace(run=None))
        except ka.HarnessError:
            raise ka.HarnessError("no game is running; call start_game first") from None
        return self.run_dir

    def _running(self, run_dir: Path) -> None:
        if (run_dir / "exit.json").exists():
            status = ka.read_session_status(run_dir)
            raise ka.HarnessError(f"the game is no longer running ({status.get('result')}: "
                                  f"{status.get('reason')}); see {run_dir / 'summary.md'}")

    def _send(self, command: str) -> dict:
        run_dir = self._session()
        self._running(run_dir)
        ack = ka.send_command(run_dir, command)
        if not ack.get("ok"):
            raise ka.HarnessError(ack.get("error") or f"`{command}` was rejected")
        return ack

    # ---- tools --------------------------------------------------------------------------------
    def start_game(self, game: str = "", args: str = "", wait_seconds: float = 90.0) -> dict:
        if self.run_dir is not None and not (self.run_dir / "exit.json").exists():
            return {"error": f"a game is already running in {self.run_dir}; call stop_game first"}
        cfg = ka.make_config(build_dir=str(self.cfg.build_dir), emulator=str(self.cfg.emulator),
                             replay_emulator=str(self.cfg.replay_emulator), root=str(self.cfg.root),
                             game=game or os.environ.get("KYTY_GAME") or None,
                             args=" ".join(filter(None, [os.environ.get("KYTY_EMULATOR_ARGS", ""), args])))
        started = ka.start_session(cfg, boot_grace=_env_float("KYTY_BOOT_GRACE", 300.0),
                                   wait=min(max(wait_seconds, 5.0), 600.0))
        self.run_dir = Path(started["run_dir"])
        self.last_shot = None
        return started

    def stop_game(self) -> dict:
        run_dir = self._session()
        result = ka.stop_session(run_dir)
        self.run_dir = None
        result["summary"] = str(run_dir / "summary.md")
        return result

    def game_status(self) -> dict:
        report = ka.read_session_status(self._session())
        status = report.get("status") or {}
        brief = {"run_dir": report["run_dir"], "running": report["running"],
                 "uptime_s": round(status.get("uptime_ms", 0) / 1000.0, 1),
                 "host_fps": status.get("host_fps"), "guest_fps": status.get("guest_fps"),
                 "host_presents": status.get("host_presents"), "guest_flips": status.get("guest_flips"),
                 "pad_reads": status.get("pad_reads"), "last_shader": status.get("last_shader")}
        if not report["running"]:
            brief.update(result=report.get("result"), reason=report.get("reason"),
                         summary=str(Path(report["run_dir"]) / "summary.md"))
        elif not status:
            brief["note"] = "no heartbeat yet; the game is still starting"
        return brief

    def press(self, button: str, times: int = 1, ms: int = 120, gap_ms: int = 300) -> dict:
        names = ka.normalize_buttons(button)
        times = max(1, min(int(times), MAX_PRESS_TIMES))
        ms = max(16, min(int(ms), 10000))
        for index in range(times):
            self._send(f"press {names} {ms}")
            # Wait until the press is over, plus a gap, so repeated presses register separately.
            time.sleep((ms + (max(0, int(gap_ms)) if index + 1 < times else 50)) / 1000.0)
        return {"pressed": names, "times": times, "ms": ms}

    def hold(self, button: str) -> dict:
        names = ka.normalize_buttons(button)
        self._send(f"hold {names}")
        return {"holding": names}

    def release(self, button: str = "all") -> dict:
        names = "all" if button.strip().lower() == "all" else ka.normalize_buttons(button)
        self._send(f"release {names}")
        return {"released": names}

    def stick(self, side: str, x: float, y: float, ms: int = 1000) -> dict:
        side = side.strip().lower()[:1]
        if side not in ("l", "r"):
            raise ka.HarnessError("side must be 'left' or 'right'")
        x, y = max(-1.0, min(float(x), 1.0)), max(-1.0, min(float(y), 1.0))
        self._send(f"stick {side} {x:g} {y:g} {max(0, int(ms))}")
        return {"stick": side, "x": x, "y": y, "ms": int(ms)}

    def trigger(self, side: str, value: float = 1.0, ms: int = 500) -> dict:
        side = side.strip().lower()[:1]
        if side not in ("l", "r"):
            raise ka.HarnessError("side must be 'left' or 'right'")
        value = max(0.0, min(float(value), 1.0))
        self._send(f"trigger {side} {value:g} {max(0, int(ms))}")
        return {"trigger": side, "value": value, "ms": int(ms)}

    def send_raw(self, command: str) -> dict:
        return self._send(ka.normalize_command(command))

    def wait(self, seconds: float) -> dict:
        run_dir = self._session()
        end = time.monotonic() + max(0.0, min(float(seconds), MAX_WAIT_SECONDS))
        while time.monotonic() < end and not (run_dir / "exit.json").exists():
            time.sleep(min(0.25, max(0.0, end - time.monotonic())))
        return self.game_status()

    def screenshot(self, name: str = "") -> Path:
        run_dir = self._session()
        self._running(run_dir)
        self.shot_count += 1
        path = ka.take_shot(run_dir, name or f"mcp_{self.shot_count:04d}")
        self.last_shot = path
        return path

    def look(self, question: str = "") -> dict:
        shot = self.screenshot(f"look_{self.shot_count + 1:04d}")
        try:
            described = vision.describe(shot, question)
        except vision.VisionError as error:
            return {"error": str(error), "screenshot": str(shot)}
        return {"description": described["answer"], "screenshot": str(shot),
                "model": described["model"], "seconds": described["seconds"]}

    def save_reference(self, name: str, box: list[float], screenshot: str = "") -> dict:
        source = Path(screenshot) if screenshot else self.last_shot
        if source is None or not source.is_file():
            raise ka.HarnessError("no screenshot yet; call screenshot or look first")
        clean = re.sub(r"[^A-Za-z0-9_.-]", "_", name).strip("._") or "ref"
        made = ka.make_reference(source, REFS_DIR / f"{clean}.png", box=[float(v) for v in box])
        made["scenario_ref"] = f"refs/{clean}.png"
        return made


def build_server(tools: Optional[GameTools] = None):
    """The MCP server, with the SDK imported only here."""
    try:
        from mcp.server.mcpserver import Image, MCPServer  # mcp 2.x
    except ImportError:
        from mcp.server.fastmcp import FastMCP as MCPServer, Image  # mcp 1.x

    tools = tools or GameTools()
    server = MCPServer("kyty", instructions=INSTRUCTIONS)

    def guarded(function, *args: Any, **kwargs: Any) -> Any:
        try:
            return function(*args, **kwargs)
        except (ka.HarnessError, vision.VisionError) as error:
            return {"error": str(error)}

    @server.tool()
    def start_game(game: str = "", args: str = "", wait_seconds: float = 90.0) -> dict:
        """Launch the game in the emulator. `game` is the game directory (default: KYTY_GAME or
        kyty_run.sh); `args` adds kyty_emulator options. Returns once it is running."""
        return guarded(tools.start_game, game, args, wait_seconds)

    @server.tool()
    def stop_game() -> dict:
        """Quit the game and return the run's verdict and summary path."""
        return guarded(tools.stop_game)

    @server.tool()
    def game_status() -> dict:
        """Whether the game is running, frame rates, and why it stopped if it did."""
        return guarded(tools.game_status)

    @server.tool()
    def press(button: str, times: int = 1, ms: int = 120, gap_ms: int = 300) -> dict:
        """Press a button `times` times (e.g. button="rb", times=2). Combine with '+': "l1+r1".
        `ms` is how long each press is held, `gap_ms` the pause between presses."""
        return guarded(tools.press, button, times, ms, gap_ms)

    @server.tool()
    def hold(button: str) -> dict:
        """Hold a button down until release is called."""
        return guarded(tools.hold, button)

    @server.tool()
    def release(button: str = "all") -> dict:
        """Release a held button, or "all"."""
        return guarded(tools.release, button)

    @server.tool()
    def stick(side: str, x: float, y: float, ms: int = 1000) -> dict:
        """Push the left or right stick. x,y in -1..1 (y=+1 forward/up) for `ms` milliseconds;
        ms=0 holds it until the next stick call."""
        return guarded(tools.stick, side, x, y, ms)

    @server.tool()
    def trigger(side: str, value: float = 1.0, ms: int = 500) -> dict:
        """Pull the left or right trigger (L2/R2) to `value` (0..1) for `ms` milliseconds."""
        return guarded(tools.trigger, side, value, ms)

    @server.tool()
    def send_raw(command: str) -> dict:
        """Send any automation command, e.g. "stick l 0 1 2000" or "reset"."""
        return guarded(tools.send_raw, command)

    @server.tool()
    def wait(seconds: float) -> dict:
        """Let the game run for up to 120 seconds, then return its status."""
        return guarded(tools.wait, seconds)

    @server.tool()
    def screenshot(name: str = "") -> Any:
        """Screenshot of the next frame the game presents, as an image."""
        try:
            path = tools.screenshot(name)
        except ka.HarnessError as error:
            return {"error": str(error)}
        return [Image(path=path), f"saved to {path}"]

    @server.tool()
    def look(question: str = "") -> dict:
        """Ask the local vision model what is on screen: screen type, text, selected item, button
        prompts, and whether the player is controllable. Optionally ask a specific question."""
        return guarded(tools.look, question)

    @server.tool()
    def save_reference(name: str, box: list[float], screenshot: str = "") -> dict:
        """Crop the last screenshot (or `screenshot`) to `box` = [x0, y0, x1, y1] fractions of the
        frame and save it as scenarios/refs/<name>.png for a scenario `until` step."""
        return guarded(tools.save_reference, name, box, screenshot)

    return server


def main() -> None:
    build_server().run("stdio")


if __name__ == "__main__":
    main()
