# Graphics regression testing

Kyty can record and compare exact presented frames at selected zero-based presentation ordinals.
The comparison happens on the normalized `PreparedFrame` image before the swapchain blit, so window
size and desktop composition do not affect regular game frames. Synthesized blank frames inherit
the prepared-frame pool format and should only be compared in the same pinned swapchain setup.

Record a baseline:

```text
kyty_emulator --game <game> --regression-record baselines/game.json \
  --regression-frames 60,120,300
```

Compare a later build:

```text
kyty_emulator --game <game> --regression-compare baselines/game.json \
  --regression-report artifacts/game-report.json --regression-frames 60,120,300
```

For unattended jobs, use the timeout wrapper. It removes a stale report before starting, enforces
the deadline, and validates that the new report is complete and contains exactly the requested
matching frames:

```text
python tools/run_graphics_regression.py --emulator <kyty_emulator> --game <game> \
  --baseline baselines/game.json --report artifacts/game-report.json \
  --frames 60,120,300 --timeout-seconds 300
```

The emulator exits with code `0` when every selected frame matches and `2` for a mismatch, missing
frame, invalid baseline, or capture failure. It exits as soon as all selected frames have been seen
unless `--regression-exit-on-complete false` is specified. Raw frames, PPM previews for 8-bit RGBA
formats, and PPM difference images are written beside the manifest by default. Use
`--regression-save-raw false` when only hashes and metadata are needed.

The oracle is intentionally exact: it hashes tightly packed source bytes with XXH3-128 and also
checks width, height, pitch, byte size, and format. Baselines should therefore be recorded and
compared with the same GPU model, driver, Vulkan settings, game state, and emulator configuration.
Use a separate baseline per pinned CI graphics environment. A future tolerant-image policy can be
added alongside the exact policy without weakening this signal.

Recording refuses to overwrite an existing baseline. This avoids silently approving a rendering
change; delete or move a baseline only as an explicit review action.

## Commands-only GPU traces

`--gpu-capture <path>` writes a versioned, little-endian trace of top-level graphics submissions,
compute submissions, flip-preparation markers, and graphics-suspend requests. A suspend marker is
recorded when the guest calls its suspend point, before Kyty waits for the GPU to become idle; it is
not a completed-fence event. Every event has an XXH3 checksum and is flushed immediately, so all
complete records written before a crash remain durable. A final record interrupted mid-write is
reported as truncated by the loader.

This format advertises the `commands_only` capability. It is not a standalone replay capture:
indirect command buffers, guest-memory resources, aliases, texture contents, vertex/index buffers,
and GPU-written state are not materialized. A replayable format must add range-scoped resource
readback and guest-memory topology rather than treating top-level PM4 bytes as sufficient.

## CI

The Windows workflow builds the emulator and the two focused test executables, then runs tests for
trace round trips, corruption/truncation handling, exact frame matches, mismatches, difference
images, incomplete runs, and baseline overwrite protection. Game-specific jobs can then run the
wrapper above on a pinned GPU runner and upload the report directory. Input scripting and
save-state-driven checkpoints remain game-specific; use stable startup state before relying on a
presentation ordinal.
