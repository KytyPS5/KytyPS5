#!/usr/bin/env python3
"""Stands in for kyty_emulator in the autoplay harness tests.

It speaks the same automation protocol as the real emulator (commands.txt in, events.jsonl and
status.json out, PNG screenshots) around a tiny pretend game, and misbehaves on request.

$FAKE_BEHAVIOR picks the behaviour:

    pass          menu, then gameplay after two `press cross`; the left stick scrolls the world
    frozen_world  like pass, but the stick does nothing (present, yet not controllable)
    stuck         never leaves the menu
    ignore_quit   like pass, but `quit` is ignored (the harness has to kill it)
    shader_abort  prints a shader fatal error (text from $FAKE_FATAL_FILE) and exits 65
    other_abort   prints a non-shader fatal error and exits 65
    segv          dies of SIGSEGV
    exit0         exits 0 after a moment
    hang          like pass, then stops presenting after ~2 s while the heartbeat continues
    boot_hang     never presents at all

$FAKE_CAPTURE_DIR, when set, is copied into --shader-capture-dir for shader_abort.
"""
import json
import os
import shutil
import signal
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw

WIDTH, HEIGHT = 320, 180
TICK = 0.02


def render_menu():
    image = Image.new("RGB", (WIDTH, HEIGHT), (10, 20, 80))
    ImageDraw.Draw(image).rectangle((110, 60, 210, 120), fill=(240, 240, 240))
    ImageDraw.Draw(image).rectangle((130, 75, 190, 105), fill=(200, 30, 30))
    return image


def render_gameplay(scroll):
    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            value = ((x + scroll) * 5 + y * 3) % 256
            pixels[x, y] = (value // 2, value, 90)
    draw = ImageDraw.Draw(image)
    # The radar: a bright disc on a dark square in the bottom-left corner.
    draw.rectangle((6, 120, 66, 175), fill=(10, 40, 10))
    draw.ellipse((14, 126, 58, 170), fill=(220, 240, 220))
    return image


def option(name, default=None):
    argv = sys.argv
    return argv[argv.index(name) + 1] if name in argv else default


def main():
    behavior = os.environ.get("FAKE_BEHAVIOR", "pass")
    auto = Path(option("--automation-dir", "."))
    log_path = Path(option("--printf-output-file", auto / "_kyty.txt"))
    capture_dir = Path(option("--shader-capture-dir", auto / "capture"))
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = open(log_path, "w", buffering=1)
    log.write("fake emulator starting\n")
    print("fake emulator starting", flush=True)

    if behavior in ("shader_abort", "other_abort", "segv", "exit0"):
        time.sleep(0.4)
        if behavior == "exit0":
            sys.exit(0)
        if behavior == "segv":
            os.kill(os.getpid(), signal.SIGSEGV)
            time.sleep(5)
        if behavior == "other_abort":
            text = ("--- Build ---\nSource build fake\n--- Fatal Error ---\n"
                    "Not implemented (a.b.c) in /repo/src/kernel/pthread.cpp:123\n")
        else:
            if os.environ.get("FAKE_CAPTURE_DIR"):
                source = Path(os.environ["FAKE_CAPTURE_DIR"])
                manifest = json.loads((source / "manifest.json").read_text())
                target = capture_dir / f"{manifest['stage']}_{manifest['hash'][2:]}_{manifest['key']}"
                shutil.copytree(source, target)
            fatal = Path(os.environ["FAKE_FATAL_FILE"]).read_text()
            text = fatal if fatal.endswith("\n") else fatal + "\n"
        sys.stdout.write(text)
        sys.stdout.flush()
        log.write(text)
        log.close()
        os._exit(65)

    (auto / "shots").mkdir(parents=True, exist_ok=True)
    commands = auto / "commands.txt"
    commands.touch()
    events = open(auto / "events.jsonl", "w", buffering=1)
    start = time.monotonic()
    state = {"presents": 0, "flips": 0, "pad_reads": 0, "presses": 0, "scroll": 0, "line": 0,
             "stick_until": 0.0, "stick_on": False, "mode": "boot", "offset": 0, "buffer": b""}
    seq = 0
    shots = []

    def emit(event):
        nonlocal seq
        seq += 1
        event["t"] = int((time.monotonic() - start) * 1000)
        event["seq"] = seq
        events.write(json.dumps(event) + "\n")

    def write_status():
        status = {"uptime_ms": int((time.monotonic() - start) * 1000), "host_presents": state["presents"],
                  "guest_flips": state["flips"], "pad_reads": state["pad_reads"], "submits": 1,
                  "schedulers": [{"gpu_tick": 7, "current_tick": 9}],
                  "recent_submits": [{"tick": 9, "op": 3, "submit_id": 11, "args": [1, 2, 3, 4, 5]}],
                  "last_shader": {"hash": "0x00000000000000b2", "stage": "cs", "age_ms": 5}}
        tmp = auto / "status.json.tmp"
        tmp.write_text(json.dumps(status))
        tmp.replace(auto / "status.json")

    def handle(line):
        words = line.split()
        ok, error = True, ""
        command = words[0]
        if command == "press":
            if words[1] == "cross":
                state["presses"] += 1
                if state["presses"] >= 2 and state["mode"] == "menu" and behavior != "stuck":
                    state["mode"] = "gameplay"
        elif command == "stick":
            if words[1] == "l" and behavior not in ("frozen_world",) and float(words[3]) != 0:
                ms = float(words[4]) if len(words) > 4 else 0
                state["stick_on"] = True
                state["stick_until"] = time.monotonic() + ms / 1000.0 if ms else 1e18
        elif command == "shot":
            shots.append(words[1] if len(words) > 1 else "shot")
        elif command == "quit":
            if behavior == "ignore_quit":
                pass
            else:
                emit({"event": "quit"})
                events.flush()
                os._exit(0)
        elif command in ("hold", "release", "reset", "status", "trace", "trigger", "stall_present"):
            pass
        else:
            ok, error = False, f"unknown command '{command}'"
        return ok, error

    def poll_commands():
        data = commands.read_bytes()
        if len(data) < state["offset"]:
            state["offset"], state["line"], state["buffer"] = 0, 0, b""
        chunk = data[state["offset"]:]
        state["offset"] = len(data)
        state["buffer"] += chunk
        while b"\n" in state["buffer"]:
            raw, state["buffer"] = state["buffer"].split(b"\n", 1)
            state["line"] += 1
            text = raw.decode().strip()
            if not text or text.startswith("#"):
                continue
            ok, error = handle(text)
            emit({"event": "ack", "line": state["line"], "cmd": text, "ok": ok, "error": error})

    emit({"event": "started", "build": "fake"})
    next_status = 0.0
    hang_at = 2.0
    while True:
        now = time.monotonic()
        elapsed = now - start
        poll_commands()
        if state["mode"] == "boot" and elapsed > 0.5 and behavior != "boot_hang":
            state["mode"] = "menu"
        presenting = state["mode"] != "boot"
        if behavior == "hang" and elapsed > hang_at:
            presenting = False
        if presenting:
            state["presents"] += 1
            state["flips"] += 1
            state["pad_reads"] += 1
            if state["stick_on"]:
                if now < state["stick_until"]:
                    state["scroll"] += 3
                else:
                    state["stick_on"] = False
            if shots:
                image = render_menu() if state["mode"] == "menu" else render_gameplay(state["scroll"])
                for name in shots:
                    path = auto / "shots" / f"{name}.png"
                    image.save(path)
                    image.save(auto / "latest.png")
                    emit({"event": "shot", "name": name, "ok": True, "path": str(path),
                          "width": WIDTH, "height": HEIGHT})
                shots.clear()
        if now >= next_status:
            write_status()
            next_status = now + 0.2
        time.sleep(TICK)


if __name__ == "__main__":
    main()
