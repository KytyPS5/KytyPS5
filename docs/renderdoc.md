# RenderDoc investigation

Baseline: `c8c15bf3` on the `nhl26` branch. The earlier `9dc25732`
compatibility change reports that device loss still occurs under RenderDoc.
This branch adds diagnostics, portable RenderDoc loading and a game-free capture
test. NHL 27 menu capture and replay were verified with RenderDoc 1.46 on the
RTX 3070. The dark-player lighting bug remains visible in the replay and is not
fixed by this branch.

## Direct launch with `--rd` on Windows

For a portable x64 RenderDoc package, copy **both** `renderdoc.dll` and
`renderdoc.json` from the same package into the directory containing
`kyty_emulator.exe`. Launch the emulator normally with your game arguments,
`--redzone` and `--rd`, then focus its window and press F1. No `renderdoccmd`
wrapper or manually configured Vulkan environment is required for this path.
Run the emulator without administrator privileges; the
[Vulkan loader ignores custom layer search paths in elevated applications](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md#exception-for-elevated-privileges).

The loader first uses an injected DLL or the installed copy found in the Windows
registry. If neither is available, it checks the **executable directory**, not
the working directory, for the portable DLL and manifest. For this fallback it
adds that directory to the active Vulkan layer search path and enables
`VK_LAYER_RENDERDOC_Capture` in this process. Existing layer/path entries are
preserved; saved environment settings and registry keys are not changed.
Automatic compatibility detection sees the selected layer.

Before this fallback was added, a direct launch using only `--rd` failed on the
test machine because portable RenderDoc had no installation registry entry.
The DLL was not loaded, and F1 logged `capture requested, but RenderDoc is
unavailable`. The earlier successful NHL captures used the injected launch
described below; they did not prove that direct `--rd` worked.

The fixed direct launch was verified on 2026-10-09 with RenderDoc 1.46, RTX 3070
and driver 617.14. With injected/launcher layer settings cleared, the emulator
loaded its adjacent DLL, enabled compatibility automatically and created the
Vulkan device successfully. The human tester pressed F1; the capture completed
with result 1 and saved a 330,902,923-byte `.rdc`. That file replayed with
`renderdoccmd replay --loops 1`, exit 0. This verifies the captured frame, not
every NHL scene or shader-debugging feature.

## Reproduce the game test

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

## Verified NHL 27 run

On 2026-10-09, emulator commit `0f8d54cf`, RenderDoc 1.46 (`e4bd23b6`), RTX 3070
and NVIDIA driver 617.14:

- Vulkan instance and device creation succeeded with RenderDoc injected.
- The human tester took two F1 captures. Both completed with
  `EndFrameCapture result=1`, producing 1,299,975,300-byte and 1,225,450,045-byte
  files. The game was closed after confirming both files were saved.
- The first capture replayed using `renderdoccmd replay --loops 1` with exit 0
  and no reported replay error.
- The first capture opened in the RenderDoc UI with the status
  `loaded. No problems detected.` Its final event was EID 25212, and the texture
  viewer displayed the NHL 27 main menu with the dark player.
- UI inspection confirmed that compute dispatch EID 11 exposes its shader
  disassembly and bindings. Buffer 95465, bytes 127616-127632, displayed four
  uint values: 64, 64, 64, 676. Graphics draw EID 947 exposed its graphics pipeline
  and fragment shader. These event/resource IDs apply only to that capture;
  they do not identify the light-culling shader.

The second capture's file creation is confirmed; its replay has not been checked.
Shader single-stepping and the light-culling inputs/outputs have not been verified.
Captures and logs remain outside Git.

### Portable launch used for this test

The portable package does not need system-wide layer registration when the
following variables are set in the launching process. Adjust paths, account
details and game location for your machine:

```powershell
$rd = 'C:\tools\RenderDoc_1.46_64'
$install = 'C:\dev\KytyPS5-renderdoc\_Build\windows\install'
$game = 'C:\path\to\eboot.bin'
$env:VK_LAYER_PATH = $rd
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_RENDERDOC_Capture'
$env:KYTY_RENDERDOC_COMPAT = '1'
$env:KYTY_RENDERDOC_DIR = Join-Path $env:TEMP 'kyty-renderdoc-captures'
$gameArgs = @(
    '--screen-width', '1280', '--screen-height', '720',
    '--user-name', 'Cryan', '--user-id', '1000',
    '--present-mode', 'Mailbox', '--readback-linear-images', 'true',
    '--sync-raw-image-buffers', 'true', '--vblank-frequency', '60',
    '--vulkan-validation', 'false', '--shader-validation', 'false',
    '--shader-log-direction', 'Silent', '--printf-direction', 'File',
    '--printf-output-file', '_kyty-renderdoc.txt', '--redzone', '--rd',
    '--game', $game
)
& "$rd\renderdoccmd.exe" capture -w -d $install `
    "$install\kyty_emulator.exe" @gameArgs
```

In RenderDoc, use **File > Open Capture** (`Ctrl+O`) and select the `.rdc` from
the capture directory. Select an event in the **Event Browser** to inspect its
pipeline and resources. **Pipeline State > CS** shows a compute dispatch's
shader and bindings; **View** opens its disassembly, and a buffer's **Go** arrow
opens its contents. The **Texture Viewer** shows captured render targets.

## What the diagnostics establish

- Whether `renderdoc.dll` was already loaded, installed or loaded as a portable copy,
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
with RenderDoc available. For a direct portable test, copy `renderdoc.dll` and
`renderdoc.json` beside the test executable and run
`shader_recompiler_compute_tests.exe --renderdoc-only` normally. Alternatively,
use the injected launch below. This uses the production request/guest-flip capture
flow and checks GPU results for buffer load/store, buffer atomics and wave32 64-bit LDS
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
- Direct portable loading was reproduced failing with exit 2 before the fix,
  even with the DLL and manifest beside the test executable. After the fix,
  the same direct selector passed all three GPU checks and saved a 311,887-byte
  capture, which replayed with exit 0. This run used no injection, installation
  registry entry, manually selected Vulkan layer or compatibility override;
  its working directory differed from the executable directory.
- `--thread-dimensions-only`, `--buffer-cache-range-only` and `--scheduler-only`
  passed on the GPU without RenderDoc.
- The full `shader_cfg` and `shader_recompiler_compute` CTest suites fail on both
  the unchanged `c8c15bf3` baseline and this branch. Their first failures are
  `S_SETPC_B64 jump table did not select dispatcher fallback` and
  `Pm4NativeTargetGeometry ... a removed GCN register has a handler or a native
  packet is not handled`, respectively. These existing failures prevent claiming
  that the full suites pass.

The standalone capture contains compute work, so its replay preview has no game
image. The separate NHL run above verified a main-menu capture; a team-select
frame and an in-match frame have not been inspected.

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
