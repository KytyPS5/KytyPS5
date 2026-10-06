# NVIDIA DLSS (experimental)

The launcher exposes **NVIDIA DLSS** in the graphics settings, globally and per
game: Off (default), Quality, Balanced, Performance, UltraPerformance and DLAA.
The command line accepts the same names: `--dlss Quality`. **Output resolution**
selects the reconstruction target. **Render scale** (25–100%, default 100%)
reduces the dimensions of supported guest raster passes. **DLSS Frame Generation**
is a separate toggle, off by default, and can also run with Super Resolution off.

Global settings are inherited by games unless a per-game setting overrides them.
Off is the default. In a build with DLSS enabled, select a mode and start the
game; no game-name detection, patch or temporal-input setup is required.

Selecting a mode runs the NGX Vulkan Super Resolution backend on the main
VideoOut frame, independently of the game title. The emulator generates its
temporal inputs on the GPU; no game-specific adapter is required. A compatible
NVIDIA RTX device and the installed NGX runtime are required. Frame Generation
uses the optional Streamline Vulkan backend described below. Ray Reconstruction
is outside this integration.

This is **experimental final-frame reconstruction**. Supported opaque draws
capture the actual clip-space positions produced by the guest vertex shader,
retain them across consecutive MAIN frames, and export current-to-previous motion
and fragment depth through a spare attachment in the same draw. Hardware depth
tests and fragment discard apply to that attachment too. This generates motion
from guest geometry; it does not identify or directly consume a game's velocity
texture. A four-level color pyramid estimates image-space motion for pixels
without validated geometry history. Both paths convert motion to render pixels.
The final color is resampled with Halton jitter. At 100% render scale, its
existing resolution is retained within the SDK's supported dynamic-resolution
range. Super Resolution is skipped when the output is no larger than that
native final frame; explicit DLAA still evaluates the model. A completed,
unchanged full-surface raster pass supplies its actual reduced color directly,
avoiding resampling the native-sized nearest copy. Its resolution is retained
within the SDK's input range too. Other materialized surfaces retain the SDK's
recommended input size because their dimensions do not prove their raster size.
Covered geometry supplies actual fragment depth, including reversed-depth
normalization. Uncovered pixels use an explicit neutral image plane. A per-pixel
current-color bias rejects poorly matched estimated history.
Consequently this does not provide the same inputs or guarantee the same image
quality as a native game integration: fast motion, occlusion, thin details and
transparency can produce artifacts. HUD already drawn into the guest frame is
processed too; the separate VideoOut overlay bus remains outside reconstruction.

**At 100% render scale, guest color resolution is unchanged.** Geometry capture and
presentation add GPU work and do not guarantee an FPS improvement.
Performance/UltraPerformance select the NGX preset and supported input range;
they do not make the game render fewer pixels.
Use Off with Frame Generation disabled to compare the original frame without
geometry instrumentation. There are no game-name checks in this path.

Below 100%, the rasterizer uses smaller color/depth attachments and adjusts
viewports, scissors and fragment position inputs to preserve guest coordinates.
Completed passes materialize their results into the native-sized cache image
before later texture, storage or CPU consumers can observe it. Guest allocation
sizes and compute shader grids remain unchanged. Unsupported formats, MSAA,
attachment feedback, reinterpreted views and very small targets stay native.
Copies into reduced attachments are skipped when the pass clears all their
aspects; LOAD and mixed depth/stencil operations preserve existing content.
This reduces raster pixel count but adds attachment copies; it does not guarantee
lower total GPU time. Lower scales lose detail. For a 1920x1080 supported pass,
50% rasterizes at 960x540; its resulting final frame is reconstructed to the
selected output resolution using the chosen DLSS mode.

```text
--dlss Quality --render-scale 50 --screen-width 2560 --screen-height 1440
--dlss-frame-generation true
```

### What changes when DLSS is enabled

| Stage | Off | Quality / Balanced / Performance / UltraPerformance | DLAA |
| --- | --- | --- | --- |
| Guest raster passes | Controlled separately by Render scale | Controlled separately by Render scale | Controlled separately by Render scale |
| Main final color | Copied for ordinary presentation | Actual source retained within the SDK's input range; recommended size for unproven materialized surfaces | Resampled to the output size, then reconstructed at that size |
| Temporal inputs | Generated when Frame Generation is requested | Captured geometry motion/depth where valid; GPU optical flow/neutral depth elsewhere; resampling jitter, history rejection | Same emulator-generated inputs |
| Prepared output | Original presentation format and size | RGBA16F at the configured screen resolution | RGBA16F at the configured screen resolution |
| HUD inside the main frame | Original presentation | Processed with the scene | Processed with the scene |
| Separate overlay bus and host overlay | Composited by the presenter | Composited after reconstruction | Composited after reconstruction |
| Extra work | Ordinary presentation | Input generation and NGX evaluation | Input generation and NGX evaluation |

For example, with a native 1920x1080 guest frame at 100% render scale and a
1280x720 configured output, Quality retains the original frame for ordinary
presentation instead of shrinking it to 853x480 and reconstructing it again.
For an output larger than the source, reconstruction retains as many existing
source samples as the SDK's supported input range permits.
These input sizes are SDK-dependent; query them instead of
hardcoding a ratio. The configured screen resolution selects the DLSS target;
the swapchain can still scale that image to the window's drawable size.

This change evaluates the NVIDIA model and presents its computed pixels; it
does not merely load NGX or change a settings label. It may change edge/detail
appearance and temporal stability, but visual improvement is not guaranteed.
The generated jitter shifts samples of the existing final image; it does not
jitter the guest camera or expose additional guest-rendered scene information.
The extra GPU work can reduce throughput when the workload is already limited
by GPU execution. A vblank-capped run can hide that cost in its average FPS.

When a mode is requested, the title displays `DLSS: active` only for frames with
a successfully evaluated DLSS output; fallback frames display `DLSS: inactive`.
When the current source (after render scale) already covers the output
resolution, Super Resolution has nothing to upscale and the title shows
`DLSS: bypassed, source >= output`; raise the output resolution or lower the
render scale below the output size to reconstruct.
Opt-in CSV traces record actual evaluations and presentation intervals; see
[frame-pacing.md](frame-pacing.md). SDK availability alone does not mean DLSS
is affecting gameplay.

## Build

The official [NVIDIA/DLSS SDK](https://github.com/NVIDIA/DLSS) is included as the
`3rdparty/DLSS` Git submodule, pinned to the tested revision
`374959484e79a640feaba44c93ac8cfb0a03f5b5`. A recursive clone of this repository
initializes it with the other dependencies. For an existing checkout:

```powershell
git submodule update --init --recursive 3rdparty/DLSS
cmake -S . -B _Build/windows -DKYTY_ENABLE_DLSS=ON
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

Use the existing clang-cl / Visual Studio developer environment and Qt build
configuration described in the README. Linux x86_64 also supports the SDK;
substitute your Linux build directory. CMake defaults to `3rdparty/DLSS`; an
optional `KYTY_DLSS_SDK_ROOT` override can point to a custom SDK checkout. If an
existing build caches the old `_Build/dlss-sdk` path, clear it once with
`cmake -S . -B _Build/windows -U KYTY_DLSS_SDK_ROOT -DKYTY_ENABLE_DLSS=ON`.
Unsupported platforms can build with
`KYTY_ENABLE_DLSS=OFF` (default). Without the SDK, the launcher disables the DLSS
selector and the engine logs a fallback if DLSS is requested on the CLI.

Windows clang-cl builds with DLSS enable ASLR (`/DYNAMICBASE /HIGHENTROPYVA`).
With ASLR disabled, enabling NGX's Vulkan extensions reproducibly caused
`vkCreateDevice` to return `VK_ERROR_INITIALIZATION_FAILED` on the tested NVIDIA
driver, even though the same device initialized successfully without DLSS.
The engine and GPU test share this linker configuration so that the test covers
the actual executable layout.

The build copies the **release** Super Resolution runtime next to the engine,
launcher and GPU test, together with NVIDIA's license. Installation includes
both the runtime and license. The SDK is initialized by Git's submodule commands;
CMake does not download it. It is not covered by Kyty's license. Its distribution
terms are in `3rdparty/DLSS/LICENSE.txt`. This software contains source code
provided by NVIDIA Corporation.

## Frame Generation

Enable the independent Windows clang-cl backend with an extracted official
[Streamline SDK release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1).
The current build uses matching v2.14.1 headers and production DLLs, on an NVIDIA
RTX 4090 Laptop GPU. The Streamline release remains a separate dependency;
its SDK files are not fetched automatically or committed.

```powershell
cmake -S . -B _Build/windows -DKYTY_ENABLE_DLSS_FG=ON -DKYTY_STREAMLINE_SDK_ROOT="$PWD/_Build/streamline-release"
cmake --build _Build/windows --target launcher dlss_gpu_tests
ctest --test-dir _Build/windows -R '^dlss_frame_generation' --output-on-failure
cmake --install _Build/windows --prefix "$PWD/_Build/windows/install"
```

The build deploys the signed production interposer, common/DLSS-G/Reflex/PCL
plugins, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll` and license notices. A supported
device, driver, Windows configuration and runtime are required. The backend
checks Streamline support instead of assuming availability from the GPU name.
After rebuilding, reinstall before using `install/launcher.exe`; its companion
engine and runtime DLLs must come from the same build. Installation preserves
existing game settings and saves.
It intercepts Vulkan instance/device/surface/swapchain/present operations,
enables Reflex, tags frame-owned depth/motion snapshots and supplies per-frame
constants. If the selected device lacks `VK_KHR_present_id`, Streamline is shut
down before device creation and the engine uses native Vulkan presentation.
Only new MAIN guest frames advance generation history. Cached guest
frames retain an already active generation mode for overlay refreshes without
retagging inputs or counting another guest frame. Blank and paused presentations,
missing inputs and the Off setting disable generation. Mode changes recreate
the swapchain; when generation is active, Vulkan uses Immediate presentation
because the SDK does not support VSync for Vulkan Frame Generation.

Reflex sleep and prepared-frame retirement run before acquiring the renderer
mutex, allowing guest work to continue during pacing. On devices with at least
two universal queues, FG uses a separate queue for presentation in the same
family. Each presentation waits on the producer timeline tick of its MAIN and
overlay frames; later guest rendering can run independently. Devices with only
one queue retain the shared queue. Vulkan uses `eBlockNoClientQueues`; frame-owned input
snapshots wait on the SDK completion timeline before reuse or destruction.
The prepared-frame pool first chooses compatible images whose presentation
and FG work have completed. If only another descriptor is ready, it uses that
frame before waiting on a compatible one. Readiness queries are nonblocking;
the lifetime waits remain in place before reuse.
When the game loses foreground focus, FG is disabled and the swapchain is
recreated for ordinary presentation. Unused FG snapshots are not prepared in
the background. This avoids paying interpolation overhead while the SDK is
bypassing generation. Foreground focus enables generation on the next eligible
guest frame.
With Super Resolution inactive, input generation skips unused color/mask
writes and uses the actual source resolution for depth/motion instead of
resampling them to the output resolution.

The inputs share Super Resolution's partial geometry coverage and estimated
fallback limitations. Guest HUD
and overlays present in the backbuffer can exhibit interpolation artifacts.
Generation adds display frames without increasing game simulation FPS. The
existing CSV continues to count guest submissions. With FG enabled, the
window title's `fps` (tagged `[FG]`) reports the Streamline display-frame count
per second, including generated frames; it is not measured scanout.
Activation
is logged only after Streamline reports additional frames actually presented.

On Windows, closing a game with FG requested preserves the emulator's emergency
persistence and flushes buffered files, then terminates its own process without
DLL detach callbacks. The existing fast-exit route leaves guest threads running;
normal DLL teardown in that state reproduced an access violation inside
`nvngx_dlssg.dll`. Normal presenter destruction, used when producers have stopped,
drains presentation and FG inputs and disables generation before releasing images.

**Vulkan validation and Frame Generation cannot currently run together.**
Both tested SDK versions (2.10.3 and 2.14.1) produced internal image-layout errors,
also reported in [Streamline issue #84](https://github.com/NVIDIA-RTX/Streamline/issues/84).
The emulator preserves validation and falls back to ordinary presentation when
validation is requested. Super Resolution and render scale remain available.
The interpolation runtime test runs without Vulkan validation; its assertion
requires the SDK's actual display-frame count to exceed real submitted frames
and verifies switching off. A separate test checks validation-enabled fallback.
This is distinct from the other GPU tests, which run with validation.

**The window must remain focused.** The SDK deliberately bypasses interpolation
in the background. The integration tests focus their own window while measuring;
three consecutive production-runtime runs passed with Quality and with Super
Resolution Off. Each reported 357–358 display frames for 180 actual submissions.
The test compares against all SDK submissions, including presentation retries,
so retries cannot pass as interpolation. It also verifies Off, re-enable, output
resize and invalid-input fallback. These are runtime and lifetime checks, not a
claim of native-game image quality or an increase in game simulation FPS.

For runtime diagnosis, `KYTY_DLSS_FG_DEBUG=1` enables verbose SDK logging. Official
development DLLs may be used in a separate diagnostic directory; deployment uses
the signed production DLLs only.

## Presentation and optional native scene inputs

The normal call `Presenter::PrepareFrame(command, video_out_info)` generates
inputs automatically. It accepts single-layer, single-sample 2D final color in
RGBA8/BGRA8 UNORM or SRGB, packed RGB10A2 UNORM, RGBA16F or RGBA32F, with linear
sampling support. SRGB sources retain the guest's already encoded color through
a UNORM view. Unsupported sources, unavailable NGX or rejected input settings
retain ordinary presentation. Size/format/mode changes, rejected main frames and
gaps longer than 250 ms reset history. Motion estimation stays entirely on the
GPU; no frame readback or CPU guest-memory scan occurs in this path.

Geometry capture currently supports ordinary vertex/fragment stages, one opaque
MRT0 output, single-sample/layer views and spare varying slots 26/27 plus MRT7.
Mesh/tessellation, blending, fragment depth/sample-mask exports and occupied
interfaces retain the estimated path. A draw sampling a previously rendered or
storage image cannot advertise its quad as the sampled scene's motion/depth.
Unrelated color writes invalidate its guide; arbitrary post-processing and copies
do not propagate scene motion. Draw identities include shaders, vertex/index
resources, draw offsets, topology and viewport; ambiguous duplicates and missing
previous vertices invalidate history. Position buffers retain at most 64 MiB;
guide images retain at most 128 MiB, with GPU-completion retirement. This is a
general renderer path, but it does not guarantee native coverage of every game's
final frame, object identity across reused buffers, camera-cut detection, true
scene jitter, pre-HUD inputs or native-integration image quality.

Call `Presenter::PrepareFrame(command, video_out_info, &inputs)` once per new
guest frame to override generated inputs with a native scene adapter.
`DlssFrameInputs` must then describe actual scene color, depth and
two-channel motion vectors from the same frame, before HUD composition. Depth
and motion dimensions must match the color dimensions. Supply camera jitter in
render pixels and motion scales converting vectors to render pixels. Motion
vectors must exclude camera jitter. Set the inverted-depth/HDR flags according
to the game and reset history after scene cuts, pause/resume or discontinuities.

Inputs must be single-sample, single-layer 2D sampled images, already updated in
the texture cache, produced on the renderer's command queue. Supported depth
formats are D16, D32F, R16 UNORM and R32F; motion formats are RG16F and RG32F.
Color formats are RGBA8 UNORM, BGRA8 UNORM, RGBA16F and RGBA32F. The adapter must
also arrange game HUD composition after the reconstructed scene (for example,
using a separate VideoOut overlay layer).
The integration owns image lifetimes until the recording GPU tick completes.
The selected launcher resolution is the output resolution. Query
`DlssProcessor::OptimalInputExtent` for the SDK's recommended scene resolution;
the game adapter must actually arrange rendering at a supported input size.
Changing the launcher option does not rewrite a game's render targets.

The separate Render scale option changes host rasterization of supported passes;
it does not change the game's declared texture dimensions or render graph.

The backend creates an RGBA16F storage output, tracks Vulkan image transitions,
checks NGX capabilities and the SDK's resolution range, recreates features on
size/mode/flag changes, and defers release until queued GPU work completes.
Missing or invalid inputs reset history and retain ordinary presentation.
Failure while recording DLSS fills the prepared output from the original image.
Evaluation happens during frame preparation; repeated presentation of a cached
frame and host overlays do not advance the DLSS history.

## Verification

```powershell
cmake --build _Build/windows --target dlss_gpu_tests prepared_frame_selection_tests
ctest --test-dir _Build/windows -L headless --output-on-failure
```

The `headless` selection runs DLSS GPU checks without creating or focusing a
window. The broader `ctest --test-dir _Build/windows -R '^dlss_'` selection
also includes presentation and Frame Generation tests that open and focus
their windows; reserve those for a planned visual validation session.

`prepared_frame_selection` checks the free-frame selection when compatible
allocations are pending but other frames are ready. The GPU checks also verify
FG completion timeline queries, zero values, missing fences and retirement.
The GPU suite also blocks a producer submission on a host gate, submits work on
the separate presentation queue waiting for that producer tick, verifies that
the consumer cannot finish early, then checks its actual pixels after releasing
the gate. This runs without a window and with Vulkan validation.

`dlss_settings_tests` checks every mode, saved settings, global-to-game inheritance,
render scale, Frame Generation, old settings and corrupt values.
`upscale_menu_tests` opens the actual settings dialog and checks the live summary
and saved choices. Its Qt deployment includes the offscreen platform plugin.
`dlss_gpu_tests` uses the production backend on
an NVIDIA GPU with controlled synthetic scene buffers: every mode, repeated
frames, history reset, output resize, missing depth, invalid jitter and readback
of the computed output. It also evaluates every mode with emulator-generated
inputs, checks GPU motion direction/scaling on a translated textured scene,
zero motion for stationary frames, scene-cut rejection and resource retirement
on resize. Raster tests verify reduced color/depth attachments at 25%, 50%, 67%
and 100%, preservation of untouched pixels and materialization into native images.
Translated guest shaders also test geometric movement on a uniform-color scene,
first-frame depth, motion sign/scale, indexed duplicate vertices, instancing,
negative viewports, reduced rasterization, reversed depth, depth-rejected
fragments, duplicate draw rejection, bounded allocation and unchanged guest color.
It enables Vulkan validation when the layer is available
and fails on validation errors. These checks verify the backend, not the quality
or performance of emulated games.

`dlss_presentation_srgb`, `dlss_presentation_fallback` and
`dlss_presentation_pool` cover the review regressions: byte-preserving RGBA/BGRA
sRGB copies with DLSS Off, encoded-color compute resampling into RGBA16F when
DLSS rejects a frame, and stable image reuse while MAIN, overlay and blank frames
share the pool. The pool matches the actual output size, format and storage usage
before configuring a frame. Fallback resampling has no jitter or history update;
if it cannot run, ordinary source-sized presentation is restored.

`dlss_window_device` and `dlss_window_device_off` also exercise the production
window, Vulkan device and presenter initialization, following the engine's
subsystem initialization order. They cover startup with DLSS enabled and disabled.

`dlss_emulator_presentation` exercises the production presenter without a scene
adapter, reads back the actual prepared DLSS image, presents it through the
swapchain, checks repeated presentation, excludes separate overlays, and checks
mode/output changes plus changing source pixels. It fails if selecting DLSS
only creates the SDK backend without reconstructing the presented frame.

Run the presentation/DLSS/settings checks together using the command in
[frame-pacing.md](frame-pacing.md). They establish integration and resource
behavior, not compatibility, visual quality or FPS across the game catalog.
Validate runtime activation and measure performance separately with the same
scene and binary. Keep machine-specific results and graphics in the local PR
report rather than treating them as cross-game guarantees.
