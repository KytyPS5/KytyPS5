# autoplay

Drives KytyPS5 through a scenario, watches it, and says how the run ended. Everything it needs is
inside the emulator (input injection, screenshots, heartbeat), so it works on Wayland, needs no
window focus, and never touches the desktop.

```
python3 tools/autoplay/kyty_autoplay.py run --scenario tools/autoplay/scenarios/gta5_story.toml \
    --timeout 30m [--soak 300]
```

Needs Python 3.11+ and, for screenshot matching, Pillow. Everything else is the standard library.

| exit | result | meaning |
|---|---|---|
| 0 | `PASS` | controllable in the prologue; the optional soak finished with no abort or hang |
| 10 | `SHADER_ABORT` | fatal error naming a shader hash. The capture is replayed offline and disassembled |
| 11 | `OTHER_ABORT` | any other fatal exit, a signal, or an exit before the scenario finished |
| 12 | `HANG` | present count flat for `--hang-seconds` (20) while alive; gdb backtraces saved |
| 13 | `STUCK` | alive and rendering, but a step (or the whole run) timed out |
| 2 | harness error | bad arguments, broken scenario, a command the emulator rejected |

A run directory (`<build>/_Autoplay/<timestamp>/`) holds `summary.md` (read this first),
`result.json`, `stdout.txt`, `_kyty.txt` (guest log, flushed per line), `status.jsonl` (heartbeat
history), `shots/`, `capture/` (every shader program the game compiled), and for hangs `gdb.txt`.

## Commands

| command | |
|---|---|
| `run` | play a scenario, classify, exit with the code above |
| `start` | launch the emulator in the background for interactive use |
| `send "<cmd>"` | send one automation command and print its ack |
| `shot [name]` | screenshot of the next presented frame; prints the PNG path |
| `status` | print the latest heartbeat |
| `stop` | quit the emulator and write the result |
| `classify <run>` | rebuild `result.json`/`summary.md` from a run directory |
| `ref <shot> --box x0,y0,x1,y1 --out refs/x.png` | crop a screenshot into a scenario reference |

The emulator command line comes from `<build>/kyty_run.sh` when it exists (the first line that runs
`kyty_emulator`), `--game`, the scenario's `game`, and any `--arg=<one argument>` / `--args="<several>"`. The harness always adds
`--printf-direction File`, `--automation-dir`, `--automation-shot-interval` and
`--shader-capture-dir`, replacing the same options if the script had them.

## Automation commands

Written one per line to `commands.txt` (the emulator tails it); each is acknowledged in
`events.jsonl` with the 1-based line number.

```
press <btn>[+btn] [ms]      press for ms (default 120), then release
hold <btn>[+btn]            press until released
release <btn>[+btn]|all
stick <l|r> <x> <y> [ms]    x,y in -1..1, y=+1 is up/forward; recentered after ms (omit to hold)
trigger <l|r> <0..1> [ms]
reset                       release everything, recenter sticks
shot [name]                 shots/<name>.png and latest.png
status                      rewrite status.json now
trace <on|off>              log every GPU submit
stall_present <seconds>     freeze presentation, to test hang detection
quit                        flush the log and exit
```

Buttons: `cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad up down left right`.

## Playing through MCP (Claude Code + a local vision model)

`mcp_server.py` lets Claude Code play the game: it starts the emulator, presses buttons, and asks a
local [Ollama](https://ollama.com) vision model what is on screen. The repository's `.mcp.json`
registers it as `kyty` for Claude Code.

```
sh tools/autoplay/setup-mcp.sh          # private virtualenv in tools/autoplay/.venv; checks Ollama
ollama pull qwen2.5vl:3b
export KYTY_GAME=/path/to/PPSA04264     # fish: set -x KYTY_GAME /path/to/PPSA04264
claude mcp list                         # kyty should show as connected
```

The setup needs only `python3` with `venv` (Debian/Ubuntu: `sudo apt install python3-venv`); it
does not touch the system Python. `.mcp.json` starts `tools/autoplay/mcp_server.sh`, which uses that
virtualenv (or `python3`, if you installed `mcp` there yourself). Set `KYTY_GAME` before starting
Claude Code, or pass the game to `start_game`. If `kyty` does not connect, register it with an
absolute path: `claude mcp add kyty -- sh /path/to/KytyPS5/tools/autoplay/mcp_server.sh`.

### Over the web (Cloudflare Tunnel)

To let an agent that is not on this machine (for example a Claude Code cloud session) play, serve
the same tools over HTTP and put a Cloudflare tunnel in front:

```
sh tools/autoplay/mcp_server.sh --http                  # prints the key for this run
cloudflared tunnel --url http://127.0.0.1:8765          # second terminal; prints https://….trycloudflare.com
```

- The server listens on `127.0.0.1:8765/mcp` only (`--host`/`--port` change that); the tunnel
  connects to it locally, so nothing is opened on your router.
- **Every start generates a new random key** and prints it with ready-to-paste connection lines.
  Requests without `Authorization: Bearer <key>` get 401. The key is never written to disk, so
  stopping the server revokes it. Share it only with the agent you want to give control.
- The agent connects with
  `claude mcp add --transport http kyty https://<tunnel-host>/mcp --header "Authorization: Bearer <key>"`
  (or the `.mcp.json` entry the banner prints), in a session started after that.
- Anyone holding the key can start programs and press buttons on this machine until you stop the
  server. A quick tunnel's URL changes every time `cloudflared` restarts; a named tunnel keeps it.

| tool | |
|---|---|
| `start_game(game, args)` / `stop_game()` | launch (an interactive session, as `start`) / quit and get the verdict |
| `game_status()` | running?, fps, presents, last shader, and why it stopped |
| `press(button, times, ms, gap_ms)` | e.g. `press("rb", times=2)`; `"l1+r1"` presses both |
| `hold`, `release`, `stick(side, x, y, ms)`, `trigger(side, value, ms)`, `send_raw(command)` | the other inputs |
| `wait(seconds)` | let the game run (up to 120 s), then status |
| `screenshot(name)` | the next frame, returned as an image |
| `look(question)` | screen, text, selection, prompts, and state; `unsure` if it cannot tell, `truncated` if the read was cut off |
| `summary()` | text of this run's `summary.md` (written when the game exits) |
| `shader(name)` | zip of one capture directory (the abort's shader, or `name`) |
| `save_reference(name, box)` | crop the last screenshot into `scenarios/refs/<name>.png` |

Button names are the DualSense ones; Xbox names work too (`a b x y lb rb lt rt start`).

Environment: `KYTY_BUILD_DIR`, `KYTY_EMULATOR`, `KYTY_GAME`, `KYTY_EMULATOR_ARGS`, `KYTY_BOOT_GRACE`
(seconds before the first frame, default 300), `OLLAMA_HOST`, `KYTY_VISION_MODEL`,
`KYTY_VISION_MAX_WIDTH` (default 1280), `KYTY_VISION_TIMEOUT` (default 120),
`KYTY_VISION_NUM_CTX` (default 8192), `KYTY_VISION_NUM_PREDICT` (default 512).

**Choosing the model.** The game and the model share the GPU. On an 8 GB card such as an RTX 2070,
keep the model small: `qwen2.5vl:3b` (the default, roughly 3 GB) or `gemma3:4b`. `qwen2.5vl:7b`
reads screens better but needs about 6 GB, so run it on the CPU or another GPU. Check what is
loaded with `ollama ps` while the game runs.

## Scenarios

TOML, steps run in order (see `scenarios/gta5_story.toml`):

```toml
[[step]]
wait = "5s"                 # or wait_frames = 300
[[step]]
press = "cross"             # also hold, release, send = "stick l 0 1 2000"
[[step]]
checkpoint = "main_menu"
[[step]]
until = { ref = "refs/radar.png", box = [0.01, 0.68, 0.22, 0.99], max_diff = 0.12, timeout = "5m", while_waiting = "press cross every 3s" }
[[step]]
verify = "controllable"
radar = { ref = "refs/radar.png", box = [0.01, 0.68, 0.22, 0.99] }
stick = "l 0 1 3000"
```

A `box` of a live screenshot, shrunk to 32x32 gray, must differ from the reference by at most
`max_diff` (mean absolute difference, 0 = identical). `verify = "controllable"` checks that the
radar is visible and that holding the stick changes the frame more than standing still does. A
scenario with no `verify` step can never PASS.

## Tests

```
python3 -m unittest discover tools/autoplay/tests
```

`test_classify.py` classifies fixture logs, `test_scenario.py` covers scenarios and matching, and
`test_run_fake.py` runs the real harness against `tests/fake_emulator.py`, a stand-in that speaks the
automation protocol (it uses a built `kyty_emulator` for the shader replay checks when present).
`test_mcp_server.py` drives the MCP tools against the fake emulator and a fake Ollama, and does one
stdio round trip when the `mcp` package is installed.
