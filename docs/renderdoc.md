# RenderDoc investigation

Baseline: `c8c15bf3` on the `nhl26` branch. The earlier `9dc25732`
compatibility change reports that device loss still occurs under RenderDoc.
This branch adds diagnostics and a game-free capture test. Standalone compute
capture/replay works with RenderDoc 1.46 on the RTX 3070; NHL 27 capture/replay
still needs the human test below.

## First test

1. Record the exact emulator commit, RenderDoc product version and NVIDIA driver
   version. The in-application API version in the log is **not** the RenderDoc
   product version.
2. Launch `kyty_emulator.exe` using RenderDoc's Launch Application UI. Set the
   working directory to the emulator install directory. Use your usual NHL 27
   arguments, including `--redzone`, and add `--rd`.
3. Use `--printf-direction File --printf-output-file _kyty.txt`. Keep the full
   log, including messages without a `RenderDoc:` prefix.
4. Set `KYTY_RENDERDOC_COMPAT=1` in the launch environment for the initial test.
   This activates the existing exclusion of `VK_AMD_buffer_marker` and
   `VK_EXT_device_fault`. Its automatic detection uses layer environment
   variables and does not reliably cover every injection method.
5. Reach team select, focus the emulator window, and press F1 once. The existing
   capture begins on a guest flip and ends two guest flips later.
6. Open the resulting `.rdc` in the same RenderDoc version and check that the
   frame replays and draw calls, dispatches and textures can be inspected.

The default output is `_RenderDoc` relative to the **working directory**.
`KYTY_RENDERDOC_DIR` overrides it. The log reports the absolute capture filename
and size returned by RenderDoc.

Return the full `_kyty.txt`, RenderDoc diagnostic log if available, whether the
failure occurred before menus, before F1, during capture, or during replay, and
the capture size and replay error. Keep captures and game data outside Git.
Use separate runs for configuration changes; preserve the exact environment
used for each run, including any loop-cap or shader experiment flags.

## What the diagnostics establish

- Whether `renderdoc.dll` was already loaded or the registry load path was used,
  the DLL path, loader errors, and the bound API version.
- Vulkan instance/device creation results, GPU/driver identifiers, available
  extensions, required feature checks and enabled device extensions.
- Reported BDA capture-replay support. RenderDoc normally enables its own
  capture-replay flags; an application request of zero is not proof of a bug.
- Capture start/end state, result, capture count, output path and file size.

Loading/binding the DLL alone does not prove the Vulkan layer intercepted the
device. A saved file alone does not prove successful replay. Do not relax
renderer requirements or change capture boundaries until a test identifies
the failing stage.

## Standalone capture test

Build `shader_recompiler_compute_tests`, then run its `--renderdoc-only` selector
through RenderDoc. This uses the production request/guest-flip capture flow and
checks GPU results for buffer load/store, buffer atomics and wave32 64-bit LDS
atomics. It uses the test harness's Vulkan initialization, so it does not test
the emulator's window/device creation, presentation or game workload.

For a portable RenderDoc copy that has not registered its Vulkan layer, use
process-local variables in PowerShell (adjust both paths):

```powershell
$rd = 'C:\tools\RenderDoc_1.46_64'
$build = 'C:\dev\KytyPS5-renderdoc\_Build\windows'
$env:VK_LAYER_PATH = $rd
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_RENDERDOC_Capture'
$env:KYTY_RENDERDOC_COMPAT = '1'
$env:KYTY_RENDERDOC_DIR = Join-Path $env:TEMP 'kyty-renderdoc-smoke'
& "$rd\renderdoccmd.exe" capture -w -d $build `
    "$build\shader_recompiler_compute_tests.exe" --renderdoc-only
```

The directory must contain both `renderdoc.json` and `renderdoc.dll`. Without
layer registration or explicit layer selection, DLL injection/API binding can
succeed while `StartFrameCapture` fails. This was reproduced with the portable
package; setting the two Vulkan variables above enabled capture. These variables
also work for a portable game launch through `renderdoccmd capture` with the
emulator arguments from the first test.

Require the final `RenderDoc smoke: production capture flow and GPU readback
passed` message and a nonempty `.rdc`. The wrapper's exit code alone is insufficient
because `renderdoccmd capture -w` does not forward the application's failure code.
Then replay the filename reported in the log:

```powershell
& "$rd\renderdoccmd.exe" replay --loops 1 'C:\path\to\capture.rdc'
```

Local verification on 2026-10-09:

- Emulator and both shader test targets built successfully in Release.
- RenderDoc 1.46 (`e4bd23b6`), RTX 3070, NVIDIA driver 617.14: three smoke GPU
  checks passed; capture ended with result 1, produced one 312,147-byte file,
  and CLI replay returned 0. A rebuilt repeat also passed (311,780-byte capture).
- `--thread-dimensions-only`, `--buffer-cache-range-only` and `--scheduler-only`
  passed on the GPU without RenderDoc.
- The full `shader_cfg` and `shader_recompiler_compute` CTest suites fail on both
  the unchanged `c8c15bf3` baseline and this branch. Their first failures are
  `S_SETPC_B64 jump table did not select dispatcher fallback` and
  `Pm4NativeTargetGeometry ... a removed GCN register has a handler or a native
  packet is not handled`, respectively. These existing failures prevent claiming
  that the full suites pass.

The standalone capture contains compute work, so its replay preview has no game
image. NHL menus, F1 capture and inspection of a team-select frame remain unverified.

## Candidate changes from the other local branch

The comparison source was `KytyPS5/nhl27` at `ea208c1d`:

- `4e76e18b`: explicit buffer-cache coherence for PM4 WRITE_DATA and CPU
  FillBuffer/CopyBuffer paths, plus culling-mask diagnostics. Useful for a later
  lighting investigation; it did not resolve dark players in the recorded run.
- `5ee83e8d`: ordering back-to-back LDS operations in emulated waves. A candidate
  for shared-memory correctness, requiring focused GPU regression testing.
- `8d941afd`: partial-wave cross-lane handling and buffer/light-table diagnostics.
  Pengui's branch has separate DPP/permlane and reduction work; compare semantics
  and port individual changes instead of applying the entire commit blindly.
- `84de506e`: mixed pixel interpolation/vertex export locations and a regression
  test. Worth evaluating separately for graphics correctness.

The RenderDoc integration was identical in both local checkouts before the new
upstream compatibility commit. No changes above are included in this diagnostics
branch. Pengui's newer Opus and frame-dump work is retained.
