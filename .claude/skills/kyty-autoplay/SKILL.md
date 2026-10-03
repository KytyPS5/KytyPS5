---
name: kyty-autoplay
description: Launch KytyPS5 (GTA V, PPSA04264) and drive it with tools/autoplay or the kyty MCP server (with a local Ollama vision model), catch shader aborts, other aborts, hangs and stuck scenarios, replay a captured shader offline in about a second, and iterate until the prologue is controllable. Use when asked to run or reproduce the game, diagnose a shader abort or hang, fix a recompiler failure, or extend the autoplay scenario.
---

# KytyPS5 autonomous diagnosis loop

Goal: **controllable in the prologue.** The loop is build, run, read the verdict, fix, relaunch from
boot. The game cannot be saved mid-run, so every iteration starts from the title screen.

## The loop

1. **Build**
   ```
   cmake -S . -B _Build/linux -DKYTY_KEEP_DEBUG_SYMBOLS=ON     # once; keeps symbols for gdb
   cmake --build _Build/linux --target kyty_emulator
   ```
   Each full-emulator target recompiles the whole emulator, so use the offline replay (below) for
   shader work instead of rebuilding test binaries.

2. **Run**
   ```
   python3 tools/autoplay/kyty_autoplay.py run --scenario tools/autoplay/scenarios/gta5_story.toml \
       --timeout 1800 [--soak 300]
   ```
   No `--scenario` just watches the boot, which is enough to reproduce a boot-time abort. The game
   comes from `--game`, the scenario, or `_Build/linux/kyty_run.sh`. It prints a JSON verdict, writes
   `_Build/linux/_Autoplay/<timestamp>/`, and exits with the code below. **Read that run's
   `summary.md` first.**

3. **Branch on the exit code**

   | exit | result | what to do |
   |---|---|---|
   | 0 | PASS | done; run again with `--soak 300` if the soak was skipped |
   | 10 | SHADER_ABORT | shader loop, below |
   | 11 | OTHER_ABORT | `summary.md` has the fatal block, the log tail and a guest fault context; reproduce the cause with a test or a smaller scenario before changing code |
   | 12 | HANG | `.claude/skills/kyty-gpu-hang`; evidence is `gdb.txt` and the heartbeat in `summary.md` |
   | 13 | STUCK | the game is alive but a step timed out: interactive mode, then update the scenario |
   | 2 | harness error | wrong arguments, broken scenario, or a command the emulator rejected; fix and rerun |

4. **Relaunch from boot** after every change. Never reuse a previous run's state.

## Shader abort (10)

`summary.md` names the hash, stage and pc, replays the capture and shows the disassembly around the
failing instruction. The shader's code and the guest memory it read were captured *before* it was
compiled (`<run>/capture/<stage>_<hash>_<key>/`), so everything below works without the game.

```
E=_Build/linux/kyty_emulator
CAP=<run>/capture/cs_<hash>_<key>
$E --shader-replay $CAP                       # same fatal message as the game; IR dump with %ids
$E --shader-disasm $CAP --pc 0x86c --window 30
$E --shader-replay $CAP --no-dump             # quiet; writes $CAP/out.spv when it compiles
spirv-val --target-env vulkan1.3 $CAP/out.spv
```

- The resource-tracking failure now ends with `(op=<opcode> handle=%N <opcode> dword[i]=%M <opcode>)`.
  Those ids are lines in the `native IR before resource tracking` section of the `--shader-replay`
  output, so you can go straight to the instruction that produced the descriptor.
- Exit codes of the replay: 0 compiles, 65 fatal (the game's failure), 66 a guest read the capture
  never saw (the message lists the address), 67 invalid SPIR-V.
- The loop for a recompiler fix: edit, `cmake --build _Build/linux --target kyty_emulator`, replay
  until `REPLAY OK`, `spirv-val`, then
  `$E --shader-replay-all <run>/capture` to make sure no shader the game already compiled regressed.
- **Add a regression test.** Write the failing pattern as a few lines of RDNA2 assembly and turn it
  into a capture fixture: `tools/shader/asm2capture.py x.s tests/data/shader_capture/x --user-data ... --hash 0x..`
  (needs `llvm-mc` with the AMDGPU target), then add `shader_replay_*` ctests like the ones in
  `CMakeLists.txt`. `tests/data/shader_capture/gpu_selected_store` is a known-failing reproduction of
  the GPU-selected buffer store; when the recompiler handles it, flip
  `shader_replay_gpu_selected_store_known_failure` so it expects `REPLAY OK`.
- Captures of vertex, pixel and other non-compute stages hold a raw dump of the stage input struct,
  so replay them with the same build that captured them (the tool warns otherwise).

## Navigate with the `kyty` MCP server (preferred for exploring)

When the `kyty` MCP server is connected (`.mcp.json`; one-time setup is
`sh tools/autoplay/setup-mcp.sh`, details in `tools/autoplay/README.md`), play
the game through its tools instead of the CLI:

1. `start_game` (it uses `KYTY_GAME` or `kyty_run.sh` when no game is given). Boot can take minutes
   while shaders compile; `wait` and `game_status` until frames are presented.
2. `look` asks the local vision model to describe the screen: what kind of screen it is, its text,
   the selected item, the button prompts, and whether the player is controllable. Pass a `question`
   when you need something specific.
3. Decide from the prompts on screen, then `press` (`press("rb", times=2)`), `stick` or `trigger`.
   Give the game a moment (`wait(1)`), then `look` again.
4. The vision model is small and can misread. When an answer is surprising or you are about to do
   something irreversible, check it yourself with `screenshot`.
5. If `game_status` shows it is no longer running, read the `summary` it points to and follow the
   exit-code branch above (a shader abort is fixed offline, then relaunched).
6. When a path through the menus works, make it replayable: `save_reference` a crop that only that
   screen has, and add the matching `until` (with `while_waiting` presses) and `checkpoint` steps
   to `scenarios/gta5_story.toml`. Once free gameplay is reached, add the
   `verify = "controllable"` step. `kyty_autoplay.py run` can then repeat the whole thing without
   the model.
7. `stop_game` when you are done.

## Interactive mode (for stuck runs and for writing scenarios)

```
python3 tools/autoplay/kyty_autoplay.py start --game <dir>      # background; prints the run dir
python3 tools/autoplay/kyty_autoplay.py shot where_am_i         # prints a PNG path: Read it
python3 tools/autoplay/kyty_autoplay.py send "press cross"
python3 tools/autoplay/kyty_autoplay.py send "stick l 0 1 3000"
python3 tools/autoplay/kyty_autoplay.py status
python3 tools/autoplay/kyty_autoplay.py stop
```

Look at a screenshot after every input, then add the step to the scenario. `kyty_autoplay.py ref`
crops a reference image out of a screenshot for an `until` or `verify` step. A scenario step that
waits for something uses `until = { ref, box, max_diff, timeout, while_waiting = "press cross every 3s" }`;
see `tools/autoplay/README.md` for the format. Always `stop` a session you started.

Menus past a shader abort only become reachable once that abort is fixed, so the scenario grows one
blocker at a time. `scenarios/gta5_story.toml` is a template that stops where the game currently
stops; a scenario without a `verify = "controllable"` step cannot PASS.

## Things that are not obvious

- Input is injected inside the emulator through the pad API, never through the desktop (the window
  is Wayland, so xdotool cannot reach it). Losing focus does not pause the game.
- Fatal errors exit with status 65 (`DbgExit(321)`). The log is flushed per line while automation is
  on, so a hang or a kill keeps its tail.
- A game that never presents is reported as a hang after `--boot-grace` (default 120 s, much longer
  than the 20 s used once it has presented). GTA compiles many shaders before its first frame; raise
  it in the scenario's `[run]` if needed.
- The harness needs Python 3.11+ and Pillow for screenshot steps; `sh tools/autoplay/setup-mcp.sh`
  puts both (and the MCP SDK) in `tools/autoplay/.venv`.
- If a `git stash` in your checkout holds an earlier attempt at fixing this GTA V failure
  (`stash@{0}` in the original work), do not reuse its recompiler or renderer changes and leave the
  stash alone; its tooling has been reimplemented here.
- Do not leave emulators running: a run that exits for any reason kills its process group, but an
  interactive session needs `stop`.
