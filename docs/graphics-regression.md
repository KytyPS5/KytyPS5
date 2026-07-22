# Graphics regression testing

Kyty can record and compare exact presented frames at selected zero-based presentation ordinals.
Capture happens on the normalized `PreparedFrame` image before the swapchain blit, so window size
and desktop composition do not affect regular game frames. Synthesized blank frames inherit the
prepared-frame pool format and require the same pinned swapchain setup.

## Baselines

Choose a stable test ID that identifies the title revision and startup/input checkpoint. Record a
baseline without reusing an existing manifest or artifact directory:

```text
kyty_emulator --game <game> --regression-record baselines/game.json \
  --regression-test-id game-revision-checkpoint --regression-frames 60,120,300
```

Compare a later build:

```text
kyty_emulator --game <game> --regression-compare baselines/game.json \
  --regression-report artifacts/game-report.json \
  --regression-test-id game-revision-checkpoint --regression-frames 60,120,300
```

The baseline stores its build ID, test ID, GPU name/vendor/device/driver, Vulkan API version, and
relevant renderer configuration. Comparison rejects a different test ID or environment before any
frame is accepted. `--regression-allow-environment-mismatch true` is an explicit migration override;
the report still records both environments and `environment_match: false`.

Manifests and frame artifacts are flushed to same-directory temporary files and atomically replaced.
Recording refuses existing baseline output. Comparison also protects the baseline manifest and all
referenced baseline artifacts from report or GPU-capture path collisions. Small persistent
`.kyty-lock` sidecars provide crash-safe, cross-process output reservations; they contain no game
data and must remain beside the associated outputs.

The oracle hashes tightly packed bytes with XXH3-128 and checks width, height, pitch, byte size, and
format. Exact output still requires a pinned GPU, driver, renderer configuration, game revision, and
checkpoint. Use separate baselines for different environments. A tolerant-image policy can be added
later as a separate oracle without weakening exact comparisons.

The emulator returns `0` when every selected frame matches and `2` for a mismatch, missing frame,
invalid baseline, incompatible environment, or capture failure. It exits after the last selected
frame by default. Direct emulator runs save raw frames and supported PPM previews by default; pass
`--regression-save-raw false` for hashes only. Mismatch reports record any generated difference image
and explicitly flag missing or corrupt baseline raw evidence.

## Unattended single-game runs

The wrapper preflights all paths and arguments, gives the emulator an owned isolated working
directory, removes only previously marked runner output, retains an emulator log, strictly validates
the schema-2 baseline and report, anchors every expected hash/layout and baseline-provenance field to
the supplied baseline, and terminates the full process tree on timeout:

```text
python tools/run_graphics_regression.py \
  --test-id game-revision-checkpoint \
  --emulator <kyty_emulator> --game <game> \
  --baseline baselines/game.json --report artifacts/game/report.json \
  --work-dir artifacts/game/work --frames 60,120,300 --timeout-seconds 300
```

The wrapper saves hashes only unless `--save-raw` is used. A work directory is reset between runs
only after the wrapper has marked it as owned. `--reuse-work-dir` intentionally preserves save data
and caches; use it only when that persistent state is part of the checkpoint contract. Additional
emulator arguments go after `--`; regression-owned arguments cannot be overridden.

## Suites

`tools/graphics-regression-suite.example.json` shows the strict suite format. Paths are resolved
relative to the suite file, absolute paths are also accepted, case IDs must be unique, and the suite
runs cases sequentially so they cannot compete for the GPU. Each case receives separate work,
report, frame-evidence, log, and runner-result paths. A summary is updated after every case.

```text
python tools/run_graphics_regression_suite.py \
  --suite D:/KytyTests/suite.json --emulator D:/Kyty/kyty_emulator.exe \
  --output D:/KytyResults/current
```

Presentation ordinals are reliable only for deterministic startup sequences. Menus or gameplay that
require input must use an external deterministic input/checkpoint mechanism passed through the case's
`emulator_args`; the case ID must identify that checkpoint. The harness deliberately does not claim
that an uncontrolled ordinal represents the same scene.

## GPU-runner workflow

The `Graphics Regression Suite` workflow is manual and targets a trusted self-hosted Windows x64
runner with the custom `gpu` label. Commercial games and private baselines stay on that runner. The
workflow accepts absolute emulator and suite paths, serializes all cases, and uploads only the
summary, reports, emulator logs, and runner results, even when the suite fails or times out. Frame
evidence is a separate opt-in artifact because it can contain game imagery; work directories are
never uploaded. Protect the `graphics-regression` environment so only approved operators can start
the job. Do not enable this workflow for untrusted pull-request code on a self-hosted machine.

The normal Windows build workflow separately compiles and runs the C++ trace/frame tests and the
Python runner tests. The Python tests cover pass/fail disagreement, malformed and missing reports,
strict JSON types, stale artifact cleanup, suite validation, and timeout termination.

## Commands-only GPU traces

`--gpu-capture <path>` writes a versioned little-endian diagnostic trace of top-level graphics and
compute submissions plus flip-preparation and suspend-request markers. Existing captures are not
overwritten. Every record covers canonical metadata and payload with an XXH3 checksum, and a final
checksummed footer commits the event and command counts. Missing footer, whole-record truncation,
partial writes, count changes, corruption, and incompatible versions are rejected by the loader.

The footer marks the window-shutdown capture cutoff; submissions after that cutoff are intentionally
not included. The format advertises `commands_only` and is not a standalone replay capture. Indirect
command buffers, guest-memory resources and aliases, texture contents, vertex/index buffers, shaders,
and GPU-written state are not materialized. Replay requires range-scoped memory/resource snapshots
and guest-memory topology rather than only top-level PM4 bytes.
