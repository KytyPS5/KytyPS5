# Experimental upscaling and Frame Generation

These settings are in the launcher's **Experimental** group. Super Resolution
(SR), render scale and Frame Generation (FG) are independent. All backends use
the same renderer inputs; there are no title checks or shader-hash overrides.
Unchecking the group disables its controls; saving turns SR/FG off and restores
render scale to 100%. Existing active configurations open with the group checked.
Backend-specific rows appear only for the selected backend. The launcher uses
one generated frame with OptiScaler; the frame-count control is shown for native
DLSS FG and enabled when FG is selected.

## Settings

| Setting | Effect | Default |
| --- | --- | --- |
| Upscaler backend | Native NVIDIA NGX, or OptiScaler package | Native |
| Upscaler quality | Off, Quality, Balanced, Performance, UltraPerformance, DLAA | Off |
| Render scale | Raster resolution as a percentage of the configured output | 100% |
| Motion estimation | Hybrid geometry/optical flow, or Geometry without optical flow | Hybrid |
| OptiScaler upscaler | Auto (INI), direct Fsr, or direct XeSS | Auto |
| OptiScaler Frame Generation | Fsr or XeSS | Fsr |
| Frame Generation | Generate intermediate display frames, including with SR Off | Off |
| Generated frames | Native DLSS FG request, capped by the runtime; OptiScaler uses one | 1 |

For example, reconstruct supported raster passes at half the output dimensions:

```text
--dlss Quality --render-scale 50 --screen-width 2560 --screen-height 1440
```

SR preserves the actual final image within the SDK's input range. With 100%
render scale, a source already covering the output is presented directly; DLAA
still evaluates at the output resolution. Materialized surfaces without a
proven reduced raster use the SDK's recommended input size. Lower quality
presets alone do not reduce the guest's rendering resolution.

Render scale reduces supported raster attachments, viewports and scissors while
preserving guest coordinates and allocation sizes. Completed passes remain
available to native-size texture, storage and CPU consumers. MSAA, unsupported
formats, attachment feedback and incompatible views stay native. Smaller raster
passes save pixel work but add copies and lose detail; total cost depends on the
workload. Guest compute dispatch sizes and simulation speed are unchanged.

## Backends and runtimes

| Backend | Runtime | Build requirement |
| --- | --- | --- |
| Native SR | NVIDIA NGX / `nvngx_dlss.dll` | `KYTY_ENABLE_DLSS=ON`; supported NVIDIA GPU |
| OptiScaler Auto SR | `OptiScaler.dll`; algorithm selected in its INI | Windows; `KYTY_ENABLE_DLSS=ON` |
| Direct Fsr SR | `amd_fidelityfx_vk.dll` | Windows; NVIDIA SDKs not required |
| Direct XeSS SR | `libxess.dll` | Windows; NVIDIA SDKs not required |
| Native FG | Streamline production runtimes | Windows clang-cl; `KYTY_ENABLE_DLSS_FG=ON` |
| OptiScaler Fsr FG | `dlssg_to_fsr3_amd_is_better.dll` | Windows; `KYTY_ENABLE_DLSS=ON` for the NGX ABI |
| OptiScaler XeSS FG | `libxess_fg.dll`, optional `libxell.dll` | Windows; NVIDIA SDKs not required |

Missing runtimes, unsupported device features or failed initialization retain
ordinary presentation. Direct Fsr/XeSS SR use their Vulkan APIs and avoid the
OptiScaler NGX translation layer. The emulator still requires its normal Vulkan
1.3 renderer features; a runtime supporting a GPU does not establish emulator
support for that GPU.

### OptiScaler package

Use the [official package](https://github.com/optiscaler/OptiScaler/releases),
including backend DLLs, INI and license files. Set **OptiScaler DLL** to its full
path; an empty path resolves `OptiScaler.dll` beside the engine. Direct backends
load sibling DLLs from this directory. No proxy renaming or replacement of the
engine's NVIDIA DLLs is needed. Kyty does not distribute this package or edit its
INI.

For Auto SR, `VulkanUpscaler` in `OptiScaler.ini` chooses the output algorithm.
When keeping the package in a separate directory, set `[Libraries] OptiDllPath`
to that directory. Direct Fsr/XeSS SR bypass this INI algorithm selection.

```text
--upscale-backend OptiScaler --optiscaler-path "D:/Upscalers/OptiScaler.dll" --optiscaler-upscaler Fsr --dlss Quality --render-scale 50
```

Kyty controls FG itself. Disable competing OptiScaler presentation hooks:

```ini
[FrameGen]
Enabled=false
FGInput=nofg
FGOutput=nofg
```

For Auto FSR SR, the following is a minimal configuration without extra overlays
or vendor/extension spoofing. Re-enable DLSS when selecting a DLSS output.

```ini
[Upscalers]
VulkanUpscaler=fsr31
[DLSS]
Enabled=false
[Menu]
OverlayMenu=false
[Spoofing]
Vulkan=false
VulkanExtensionSpoofing=false
Dxgi=false
StreamlineSpoofing=false
```

### Frame Generation

FG adds display frames; it does not increase guest simulation FPS. Fsr FG
generates one intermediate Vulkan image per real frame. Its spacing wait runs
on the presentation thread after the guest flip completes, leaving the producer
free to render. XeSS FG presents through a D3D12/DXGI swapchain on the same GPU,
using shared Vulkan images and a shared fence. XeSS and DLSS-G cap the requested
generation level at the runtime's reported maximum.

```text
--upscale-backend OptiScaler --dlss Off --dlss-frame-generation true --optiscaler-frame-generation XeSS
```

Native DLSS-G requires a supported device, driver and Windows configuration.
The backend checks Streamline support, uses Reflex and a separate presentation
queue when available, and retains frame-owned inputs through the SDK completion
timeline. Missing optional extensions fall back before creating the device.
Native FG requires foreground focus and disables generation in the background.
Vulkan VSync is unsupported by this backend; Streamline paces its presents.

Native DLSS-G currently falls back when Vulkan validation is requested because
of SDK image-layout errors. SR and render scale remain available. On Windows,
the existing emergency process-exit route flushes persistence before terminating
without DLL detach callbacks while guest threads are still running. Orderly
presenter destruction drains GPU work and releases resources normally.

Cached refreshes, blank frames, failed evaluations and missing inputs do not
advance generation history or count as new guest frames. Resizing and mode
changes retire resources only after their GPU and FG work completes.

## Inputs and image quality

Supported opaque vertex/fragment draws capture previous/current positions and
fragment depth in a spare attachment. Depth tests and discard apply to this
guide. Ambiguous draw identities, resource changes and unrelated color writes
invalidate coverage; arbitrary post-processing does not propagate scene motion.
Mesh/tessellation, blending, MSAA, fragment depth/sample-mask exports and occupied
interfaces retain the estimated path. Position history is bounded at 64 MiB,
with at most 8,192 draw identities and 4 MiB of copied keys even without guest
flips; guide images are bounded at 128 MiB.

Hybrid uses a four-level optical-flow pyramid outside valid geometry coverage.
Geometry skips that pyramid; uncovered pixels use neutral depth, zero motion
and full history rejection. SR uses jittered resampling and a rejection mask.
FG-only input generation allocates no SR color/mask images and leaves color
unjittered. Pipelines are created on first use and pyramid passes bind only
their required descriptors. Switching between SR and FG-only retains compatible
motion history; extent, format, motion-mode changes and long gaps reset it.

These are final-frame inputs, including guest HUD, rather than a native game's
pre-HUD color, camera jitter and complete motion/depth. Resampling jitter adds
no new guest-rendered detail. Fast motion, transparency, occlusion and thin
details may show artifacts. The separate VideoOut overlay bus is composited
after reconstruction. No image-quality or gameplay-FPS improvement is guaranteed.

The default `Presenter::PrepareFrame(command, info)` generates inputs on the
GPU. A generic caller may supply `DlssFrameInputs`: matching sampled color,
depth and motion images produced on the renderer queue, render-pixel jitter,
motion scales and history-reset flags. The resources must remain alive through
the recording GPU tick. Evaluation runs under the renderer mutex outside
dynamic rendering, once per new MAIN frame, into single-layer RGBA16F storage.
Rejected inputs preserve ordinary unjittered presentation.

The title reports successful SR evaluation as `active`, bypass as `native`,
and fallback as `inactive`. FG display counts and guest submission counts are
separate; SDK counts are not independent scanout measurements. See
[frame-pacing.md](frame-pacing.md) for captures and comparisons.

## Build and licensing

NVIDIA options default to OFF. For native SR or OptiScaler Auto:

```powershell
git submodule update --init --recursive 3rdparty/DLSS
cmake -S . -B _Build/windows -DKYTY_ENABLE_DLSS=ON
cmake --build _Build/windows --target kyty_emulator launcher
```

Use the compiler/Qt environment described in the README. The NVIDIA SDK is
pinned by the `3rdparty/DLSS` submodule; `KYTY_DLSS_SDK_ROOT` permits an override.
Native SR also supports Linux x86_64. Other platforms retain the default build.
Windows NGX builds enable ASLR for the engine and regression executable because
NGX device creation fails without it on tested drivers.
The `guest_fixed_address_layout` regression uses the same Windows link options
as the engine and checks fixed reservations, direct mappings, and independent
user/extended mappings. Run it alongside `dlss_window_device` to check guest
allocation and NGX device creation together:

```powershell
ctest --test-dir _Build/windows -R '^(guest_fixed_address_layout|dlss_window_device)$' --output-on-failure
```

Native FG uses an extracted official
[Streamline SDK](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1):

```powershell
cmake -S . -B _Build/windows -DKYTY_ENABLE_DLSS_FG=ON -DKYTY_STREAMLINE_SDK_ROOT="$PWD/_Build/streamline-release"
cmake --build _Build/windows --target kyty_emulator launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

Headers and production DLLs must be from the same SDK release. CMake validates
the supplied SDKs and copies/installs enabled production runtimes and license
notices; it does not download them. NVIDIA code has separate terms in the SDK
licenses and is not covered by Kyty's license. This software contains source
code provided by NVIDIA Corporation. Vendored XeSS/FidelityFX interfaces retain
their license/provenance files; their runtime DLLs are not bundled.

## Verification

```powershell
cmake --build _Build/windows --target dlss_gpu_tests dlss_settings_tests upscale_menu_tests frame_pacer_tests prepared_frame_selection_tests shader_cfg_tests
ctest --test-dir _Build/windows -R '^(dlss_|presentation_|prepared_frame_selection|upscale_menu|frame_timing_analysis|shader_cfg)' --output-on-failure
_Build/windows/dlss_gpu_tests.exe --quality-cost
_Build/windows/dlss_gpu_tests.exe --input-cost
```

GPU fixtures verify motion/depth, color, detail, raster scaling, history, queues,
fallbacks and resource lifetime. FG presentation checks require actual generated
display frames and cover cached frames, Off/On and resize. They open and focus
windows. `-L headless` selects checks without a window; native FG checks run
without Vulkan validation, with a separate validated fallback test.

External-runtime tests skip when no package is configured:

```powershell
$env:KYTY_OPTISCALER_TEST_DLL = "D:/Upscalers/OptiScaler.dll"
$env:KYTY_OPTISCALER_TEST_GPU = "0"
ctest --test-dir _Build/windows -R '^(dlss_optiscaler|dlss_(Fsr|XeSS)_presentation|optiscaler_.*frame_generation)' --output-on-failure
```

Known external failures remain visible: OptiScaler's Auto Vulkan path can report
descriptor/storage-format errors and unreleased query/overlay objects; the
tested Nukem 0.130 Fsr FG path reports three first-dispatch layout errors even
when interpolated pixels match the expected midpoint. Passing direct Fsr/XeSS
tests does not certify the Auto path or every driver/package combination.
Quality fixtures and input-cost timestamps are not gameplay benchmarks. Keep
machine-specific logs, captures and comparison reports under ignored `_Build/`.
