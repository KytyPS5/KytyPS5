# Emulator regression test debt

## Ordinary compute compiler breakpoint after heavy pipelines (2026-10-04; pending)

Run `yotei-integrated-20261004-110636-presentfix-gpuav` completed the fourth
CS549 variant in 294965 ms and CSfc6f in 327456 ms, with immediate driver-cache
checkpoints after both. It reached shown356 and nonzero RGB from frame239;
`window-1108.png` visibly shows a loading spinner. It then exited
`0x80000003` at `vkCreateComputePipelines` for ordinary CS `d0c5556e1c26cb1c`
(40239 words, cooperative=false, flags0). Windows Application event1000
identifies `nvgpucomp64.dll` 32.0.16.1714, offset0x589eb2; the log does not
record DeviceLost. This is a new compiler reproduction lead, not yet an
emulator semantic defect or a valid synthetic RED.

Next: capture the pre-driver SPIR-V without dispatching it, validate it, derive
the actual descriptor/push layout, and use one bounded pipeline-only probe.
Then isolate its trigger in a reusable synthetic ordinary compute fixture
(the guest CFG has 3 loops / 36 blocks). Preserve BDA/resource bounds and
errors. No production behavior change before a meaningful synthetic RED.

## Persist expensive driver pipeline compilation (2026-10-04; host policy proved)

The warm run `yotei-integrated-20261004-104455-presentfix-gpuav` completed
CS549 variants in 154 ms (cached), 284842 ms and 291983 ms, then began a
fourth. The 16-pipeline checkpoint interval saved the second variant but did
not checkpoint the third before the task-owned stop. Required synthetic
regression: the actual checkpoint policy must preserve fast-pipeline batching
and request a checkpoint immediately after an expensive successful creation,
including when it is the first pending pipeline. Cover the interval and cost
boundaries independently; retain cache identity checks and atomic file writes.
`--pipeline-cache-checkpoint-only` got the intended native RED on the original
16-creation policy, extracted without changing its behavior
(`driver-cache-checkpoint-red-20261004.log.stderr`: completed expensive pipeline
waits for 16 creations). The unchanged test is GREEN after measuring successful
graphics/compute creation and checkpointing at 5000 ms; ordinary creations still
batch every 16. Identity, validation-mode, instrumentation and revision selectors
also passed (`_Build/logs/driver-cache-*-green-20261004.log`). Native test exe
SHA-256 `0ee283548160a3b22d6f86989960ef2481afaffee10ff94bf20cd29fa0ef5af9`.
The first build had a missing test forward declaration; that build failure is
not RED evidence. Original workload integration is now proved on source
`3eb16e4b`: CS549's fourth variant completed in 294965 ms, immediately followed
by a 356345341-byte checkpoint; CSfc6f completed in 327456 ms and immediately
checkpointed 358485977 bytes in run `...-110636-presentfix-gpuav`. Subsequent
read-back on a warm launch remains pending. This improves repeat-run persistence,
not shader semantics or menu rendering.

## Cooperative pipeline scale after local SSA reuse (2026-10-04; pending)

Next bounded synthetic regression: `shader_recompiler_compute_tests
--cooperative-wide-indirect-image-size-only` must keep a 64-operation
same-Guard arithmetic chain from emitting per-value Function stores, while
retaining spills for cross-Guard uses. Before the store correction, the native
test failed on source `1bc0d04c` with `OpStore 10 -> 74` versus its allowed
increment of eight (`_Build/logs/cooperative-local-store-red-20261004.log`).
The shader includes a guest barrier and 251-way indirect image switch; SPIR-V
validation and the existing same-Guard load check passed in that RED run.
The unchanged oracle passed after `7afddcd8` with `OpStore 9 -> 9` and
`OpLoad 530 -> 530`; multiwave LDS, cyclic barrier and cyclic scalar-address
GPUAV readback selectors also passed. The game module dropped to 566635 words
and 18629 OpStore, but the second specialization of the same shader still
blocked pipeline creation at `shown=161`.

The bounded synthetic Vulkan switch probe did not reproduce this driver
cost: 860 cases with 4 nested diamonds each, 40 Function spill/load pairs
per case and one enclosing loop produced 558338 words and created a pipeline
in 1124 ms (`_Build/logs/synthetic-switch-loop-860-20261004.log.stderr`).
The diagnostic patch is saved outside tracked source at
`_Build/analysis/synthetic-switch-pipeline-probe-20261004.patch`.
The actual CS has 32 guest loops, 844 IR blocks, 12174 SPIR-V labels and
three frequently called helpers; neither its full control flow nor the
driver-time pathology is covered by the probe. Do not use a word-count-only
threshold as evidence of menu progress.

The first 566635-word CS pipeline completed after 279999 ms in
`_Build/runs/yotei-integrated-20261004-102947-presentfix-gpuav`. The next
specialization had different bounded-SRT/table occupancy and emitted 566871
words; its pipeline creation did not complete before the 450-second frame
watchdog. Before changing specialization caching or descriptor layout, add
two synthetic materializations of one guest shader with varying active table
lengths. Verify which generated code and bounds genuinely depend on those
lengths, numerical results for valid indices, and errors for invalid indices.
Only then decide whether a shared pipeline variant can preserve the contract.

Commit `6dbee0c8` removes redundant Function loads when a cooperative
instruction is consumed inside the same Guard. The synthetic RED/GREEN and
GPUAV neighboring cases are recorded in the 09:54 UTC launch-plan checkpoint.
On the same game CS `54904…`, SPIR-V fell from 773030 to 639478 words and
OpLoad from 62605 to 29217. The bounded selective-GPUAV retry at
`_Build/runs/yotei-integrated-20261004-094551-presentfix-gpuav` still spent
300 seconds at `shown=155` inside `vkCreateComputePipelines` with no completion.
This is a remaining driver pipeline-creation blocker, not a failed SPIR-V
emission test or evidence of menu rendering.

Before another production change, create a synthetic cooperative wave64
program with many direct CFG segments and a bounded pipeline-creation probe.
The captured module has an 860-case scheduler `OpSwitch`, 87 two-case switches,
12174 labels and 886 calls. Use several segment counts to locate any steep
driver-time/memory growth without copying proprietary shader bytes or repeatedly
stressing the GPU. Keep unsupported semantics and required guest work intact.

## Large compute shader and diagnostic runtime stalls (2026-10-03; pending)

The bounded diagnostic game run
`_Build/runs/yotei-integrated-20261003-194531-presentfix-gpuav` used a
temporary four-iteration graphics-loop cap to pass the clean build's earlier
Vertex DeviceLost frontier. It reached `shown=373`, then the shown-frame
watchdog closed the task-owned process after 900 seconds without a new shown
frame. The last complete phase marker in captured stdout was `IR Normalize` for
compute shader `54904fb419d79e49` (36,869 instructions); stdout ended
midstream near `IR TrackResources`. The process was responsive and consumed up
to about 30 GB while CPU time grew. No menu was visible: a Windows desktop
screenshot in the run folder shows a loading spinner. This diagnostic result
does not establish a clean-build runtime advance or an emulator semantic RED.

The saved `54904…` code passes an offline CPU audit in under one second
(`_Build/logs/cs549-offline-audit-20261003.log`), but the registered capture
lacks runtime descriptor values and skips materialization. A second bounded
diagnostic game run with `KYTY_RESOURCE_TRACKING_TRACE=1`
(`_Build/runs/yotei-integrated-20261003-201438-presentfix-gpuav`) showed all
seven ResourceTracking phases for `54904…` finishing in milliseconds, repeatedly.
The previous partial stdout did **not** locate the stall in ResourceTracking.
This repeat instead exited 321 with `ErrorDeviceLost` at `shown=202`, despite
the same temporary cap. The cap is not a reliable workaround, and no menu was
seen. Next isolate the clean large indexed Vertex draw and the long GPUAV
pipeline/specialization path separately with bounded synthetic inputs. The
temporary cap was removed and the native installed exe restored after both
diagnostic runs.

## Divergent graphics wave64 loop with indirect images (2026-10-03; pending)

The dispatched wave64 vertex shader captured on 2026-10-04 derives EXEC from
the low byte of SGPR3 (`ESVertCount`) before copying EXEC into its scalar loop
mask. Current vertex translation initializes that byte to the fixed guest wave
size, including for a native 32-lane graphics subgroup with fewer active
invocations. AMD's RDNA2 ISA defines the byte as ESVertCount and specifies
that combined ES+GS shaders calculate real EXEC from initialized SGPRs;
Vulkan permits partially populated graphics subgroups. Add a synthetic vertex
entry-state regression for wave32/native32, wave64/native32 and wave64/native64:
the low byte must follow actual launched lanes, while unrelated SGPR3 bits
remain intact. Prove the old fixed value fails before changing translation.
Then validate a partial indexed draw with GPUAV readback. Vulkan does not
guarantee that a graphics subgroup stays within one draw; this remains a
compatibility limit of the existing wave64 partition model and must not be
presented as full guest wave64 emulation.

The synthetic entry-state regression is `shader_cfg_tests
--ngg-launched-lanes-only`. On `5a46caf4` with only host-size metadata
plumbed through, it failed as intended with `vertex ESVertCount is fixed
instead of following launched lanes` (`_Build/logs/ngg-launched-lanes-red-20261004.log`).
The shared vertex entry now uses the entry ballot's population count for
SGPR3's low byte, counting one physical half for guest wave64/native32.
The unchanged failure oracle passes after the fix. Extended checks substitute
synthetic 16/32/48/64-lane ballots, verify SGPR3's high bits, validate emitted
SPIR-V, and pass the older NGG entry fixture. Logs:
`_Build/logs/ngg-launched-lanes-spv-20261004.log` and
`_Build/logs/ngg-vertex-entry-neighbor-20261004.log`. The 48-index GPUAV
active-ballot control still passes, but its standalone test executable predates
this translation fix and is only a downstream control.

Installed emulator SHA-256 `27ab5e00a5720cd39adfcfd93045dfc3c52ea72522dc84fc1040dbac3653f9a4`.
The GPUAV-instrumented game retry
`_Build/runs/yotei-integrated-20261004-013304-presentfix-gpuav` stopped at
`shown=169` after its 300-second frame watchdog. The last shader pipeline
trace is `vkCreateComputePipelines begin` for CS `fc6f8c56eb7e168f`, whose
emitted SPIR-V has 775890 words; no `done` trace or readback followed.
The GPUAV run without shader instrumentation
`_Build/runs/yotei-integrated-20261004-014304-presentfix-gpuav` exited
`-2147483645` at `shown=0` during `vkCreateComputePipelines` for the prior
CS `b90e2024732c6111`. Neither retry reached the changed Vertex draw, so
the game's device-loss cause, menu and gameplay remain unverified. The next
bounded check must first get past those compute pipeline creation blockers;
do not count the CPU regression as game progress.

Selective instrumentation of the early `b90e…` CS allowed pipeline creation,
but the silent selective run
`_Build/runs/yotei-integrated-20261004-015524-presentfix-gpuav` remained at
`shown=154` for its full 600-second frame watchdog while creating cooperative
CS `54904fb419d79e49`. A bounded follow-up captured its pre-driver module
at `_Build/runs/yotei-integrated-20261004-020758-presentfix-gpuav/shaders/0000_new_shader_cs_54904fb419d79e49.spv`.
It has 773030 words and 197953 instructions, including 62605 `OpLoad` and
42894 `OpStore` instructions. Existing cooperative and general SPIR-V
optimizer recipes reduce it to 755593 and 750947 words respectively
(`_Build/logs/cs54904-opt-{coop,general}-20261004.log`); neither is a
measured driver-compile solution. Before changing shared emission, construct
a bounded synthetic cooperative wave64 program with an indirect image switch
and enough nested control flow to reproduce the module growth; measure its
SPIR-V size and isolated pipeline creation time on current source. Keep the
captured proprietary module outside tracked tests and source.

New bounded lead: the identified indexed draw has 119856 indices, leaving
16 after division by the native 32-lane subgroup size. In saved SPIR-V
`_Build/analysis/vs-e312-20261003.spvasm`, the loop mask starts as
`0xffffffff` in both words, then subtracts a native ballot from both words.
If a final subgroup contains only 16 executing invocations, ballot cannot
clear bits 16..31 and that loop may continue selecting absent lanes. Vulkan
does not guarantee full graphics subgroups; the actual invocation grouping
and the guest shader's mask provenance remain to be verified. Add a synthetic
Vertex mask-draining loop with a strict 32-iteration guard, subgroup ballot
and ReadLane, and distinct output when the mask remains after the guard. Run
small full and partial indexed draws with readback and GPUAV before changing
production behavior. A RED here would establish the mechanism in the host
model; it would not alone prove this is the game's device-loss cause.

The bounded synthetic probe is now in `ShaderRecompilerComputeTests.cpp`.
Native test exe SHA-256 `2e05fe92170365eda43d0aa44e5e7bf983651166155beecb87ff73d5ca291b90`:
`--wide-vertex-wave64-mask-drain-indexed-48-only` returned exit 9, with
readback red `0` instead of expected `1` after 32 iterations
(`_Build/logs/mask-drain-allones48-gpuav-20261003{,.stderr,.run.json}`).
The 96-index variant also left the mask nonzero on the prior test build,
so draw count divisible by 32 does not ensure full graphics subgroups.
The same 48-index case initialized from `Ballot(true)` instead of all ones
and passed with GPUAV (`mask-drain-active48-gpuav-20261003*`); the preexisting
96-index cross-lane/long-loop control passed on the all-ones test build
(`mask-drain-neighbor96-gpuav-20261003*`). No device loss occurred in these
bounded probes. This is a host graphics subgroup mismatch diagnostic, not a
verified guest emulation fix: the guest shader's initial mask provenance and
hardware execution of padded vertices are still unknown. In particular,
Vulkan `ReadLane` of an inactive target cannot be treated as an oracle.
Do not mask arbitrary guest scalar values or silently cap the loop. Verify
the guest ISA path before changing shared lowering.

The clean Yōtei retry at `_Build/runs/yotei-integrated-20261003-181855-presentfix-gpuav`
lost the device at `shown=200` after nonzero prepared frames 198 and 199.
An earlier SyncDiag capture points to a large indexed draw using vertex shader
`e3125617f3efc38f`, but the clean retry did not independently identify the
draw. This vertex shader has a ballot/shuffle inside a loop and a 250-way
indirect-image switch. Current synthetic wave64 vertex cases execute the same
two loop iterations in every lane, so they do not cover a changing active
mask between iterations.

Add a bounded synthetic vertex case with one or two iterations chosen per
triangle, subgroup operations inside the loop, distinct image contents and
numerical readback. Run it on the current native executable with GPU assisted
validation. A pass is a control, not the required RED for the device loss;
if it passes, narrow the captured failing draw further before any production
change. Do not infer a wave64 semantic fix from a runtime timeout or GPU reset.

The one/two-iteration variant passed numerical readback and GPUAV at 96 and
120000 unique indices on the current source. Logs:
`_Build/logs/wide-vertex-divergent-{96,120000}-gpuav-20261003.log`;
native test exe hashes `c4d3c7c5...` and `ce3e07bd...` respectively.
These are GREEN controls. The saved game SPIR-V updates two mask words after
each ballot and can make many passes through its loop. Next bounded control
uses 1–32 iterations per triangle, first at 96 indices and only then at the
119856-index draw scale if the small case completes normally. A reset or
timeout alone still cannot justify changing guest semantics.

## Formatted buffer descriptor selected at runtime (2026-10-03; pending)

Offline audit of the saved shader code at
`_Build/runs/yotei-immediate-20260926-011827/shaders/registered/cs_4d6df08d2d54e0ff_dccdf4ea82ba8496.bin`
reproduced the PC `0x284` rejection without launching the game or GPU
(`_Build/logs/cs4d-offline-audit-20261003.log`). A temporary diagnostic build
printed the pre-tracking IR to `_Build/logs/cs4d-offline-ir-20261003.log`;
the diagnostic source change was then removed. The scalar row index is guarded
by a count check in outer block `$6`, while the descriptor words are read in
block `$7` and used by formatted loads in block `$11`, inside an additional
loop whose active predicate passes through `Phi` in block `$8`. The simple
formatted-table control has one loop but no inner loop. Add a synthetic nested
loop with the same outer bounded row index, four consecutive scalar descriptor
words, and two distinct materialized rows. This tests the suspected CFG
dimension before changing the shared proof. Preserve rejection when the outer
count guard is bypassed.

The nested synthetic variant passed unchanged production code
(`_Build/logs/formatted-scalar-nested-red-20261003.log`), so nesting alone is
not the RED. Diagnostic audit of the saved shader showed its descriptor words
unproved and reported a cyclic dependency or invalid flat slot
(`_Build/logs/cs4d-proof-diagnostic-20261003.log.stderr`). That capture says
`runtime_resources_captured=false`; its translated user data was synthetic
zeroes. The same PC does not establish the same *reason* as the game run.
Repeat the bounded diagnostic game launch with runtime user data and trace the
bounded proof rejection before changing ResourceTracking. The temporary trace
patch has been removed from source.

The bounded GPUAV retry on the SRGB metadata correction,
`_Build/runs/yotei-integrated-20261003-180731-presentfix-gpuav`, passed the
previous `k8Srgb` render-target rejection and exited 321 after `shown=200`.
Compute shader `0x4d6df08d2d54e0ff` fails resource tracking at PC `0x284`:
`BUFFER_LOAD_FORMAT_X`, one formatted DWORD, with descriptor in `s20` and
vector index enabled. The existing GPU-selected buffer fallback permits only
raw DWORD x2/x3/x4 reads, so this formatted access remains rejected. No
prepared-frame readback or menu was captured; the run spent 340 seconds total,
including new GPUAV instrumented pipeline compilation.

Before changing production behavior, identify the four descriptor words and
their provenance at this PC, including any bounded table, scalar loop or
wave-uniform source. Add a synthetic formatted-load regression with distinct
numeric results for at least two descriptor choices and explicit bounds/OOB
controls. Preserve the rejection for arbitrary unbounded GPU-selected
formatted descriptors, typed stores and invalid format combinations. Run
the test RED against current source before extending the shared resource
tracking/materialization or GPU descriptor path; then retry the same game.

A first independent native control, `resource_tracking_tests
--formatted-scalar-table-only`, is GREEN on current source
(`_Build/logs/formatted-scalar-table-baseline-20261003.log`). It uses a signed
scalar loop, a 488-byte row stride, four consecutive descriptor words at
offset 200, a formatted load, and two distinct synthetic descriptor bases.
It also rejects a path that bypasses the count guard. This proves the simple
bounded-table mechanism already handles the observed geometry. It is **not**
the required RED: the game has additional EXEC/VCC masks and nested control
flow. Capture the failed descriptor's source/rejection with the existing
`KYTY_SHADER_PHASE_TRACE` before selecting the next synthetic variant.
The clean exe without the temporary Vertex cap instead lost the GPU at
`shown=200` after prepared frames 198 and 199 had nonzero RGB pixels
(`_Build/runs/yotei-integrated-20261003-181855-presentfix-gpuav`). Thus this
formatted-descriptor blocker is currently reached only in a diagnostic mode;
the clean GPU loss below is the earlier runtime frontier.

## GPUAV feature parity for synthetic vertex image selection (2026-10-03)

The first three-index synthetic vertex draw passed native GPU readback but
GPU assisted validation reported missing `shaderClipDistance` and
`vertexPipelineStoresAndAtomics` at shader-module/pipeline creation. The
production Vulkan device enables both features; the standalone test harness
did not. Before treating this as an emitter regression, enable the same
supported device features in the harness and rerun the **unchanged** bounded
three-index case with GPUAV. Then increase index counts only if the small
case has no validation errors or device loss. The captured validation failure
is a harness configuration failure, not evidence that the game's DeviceLost
has been reproduced.

The harness now checks support and enables both features, matching the
production device. The unchanged three-index GPUAV case passes on native
test exe SHA-256
`63c56550880df1cfdaa7878027439733aec815450d34bf88ceb0dc966345c2d1`;
300, 3000, 12000 and 120000 unique-index variants also pass with GPUAV
(`_Build/logs/wide-vertex-feature-*-gpuav-20261003*`). These first variants
used one texture for every descriptor, so they did not numerically prove
which table entry the vertex shader selected.

The strengthened test exe SHA-256
`a67459e310dc7bab49fc22fdd1b3fe1ce5007dcbdc12294d219f80062deae1d9`
uses 251 distinct 4×4 image contents and one unique vertex per index. The
vertex shader chooses `(VertexIndex / 3) & 255`; the final triangle's color
proves selected keys 0 (3 indices), 250 (753), 231 (3000), and 63
(120000). Key 251 (756 indices) correctly falls back to root image 0.
Native readback passes for all these cases, and GPUAV passes for 753 and
120000 without a validation error or timeout. Artifacts:
`_Build/logs/wide-vertex-distinct-{small,753,756,3000,120000}-20261003*`,
`_Build/logs/wide-vertex-distinct-{753,120000}-gpuav-20261003*`;
native build `_Build/logs/wide-vertex-distinct-build-20261003.log`.
The neighboring `--wide-indirect-image-spirv-only` case passes. This is a
GREEN control, not the game's DeviceLost RED: the synthetic shader has no
loop, uses no other guest buffers, and samples constant coordinates. Next
bounded fixture should combine a loop and additional read-only buffer
descriptors with the image lookup, then increase one dimension at a time.

The follow-up two-iteration loop and thirteen live storage-buffer reads also
pass native readback and GPUAV at 753 and 120000 unique indices (test exe
SHA-256 `e3a9e3de3f37c40b9fa154dcd1202487b55ca5f12c30d4bdd2b4c1f90963000d`,
logs `_Build/logs/wide-vertex-{loop,buffers}-*-20261003*`). The buffer
contents XOR to zero only when all thirteen loads contribute, preserving the
selected image oracle. The captured game SPIR-V has one loop and one 250-case
switch, but also `OpGroupNonUniformBallot`, `OpGroupNonUniformShuffle`, and
an explicit split at subgroup lane 32; the synthetic fixture used guest
wave32. Next compare bounded wave64 lowering on the same fixture before
adding subgroup operations or more complex buffer addressing. Do not infer
the game's loop bounds or final GPU command from this static inspection.

Test-only commit `cc3fa4e4` adds bounded wave64 vertex variants with the
same resource bindings. Native 753/120000 index cases pass numerical readback
and GPUAV with subgroup ballot plus self-lane shuffle; the prior wave64
loop/buffer control also passes. The final test exe SHA-256 is
`033b786da86c98d784a1b071aa0942e310ddd1df237737c1abd120b08a3a4336`;
logs `_Build/logs/wide-vertex-{wave64,subgroup}-*-20261003*` and
`_Build/logs/wide-vertex-cross-active-*-20261003*`. A first fixed-lane-31
fixture returned image 0 instead of image 31 at 96 vertices, but its target
lane was not guaranteed active; this was an invalid oracle, not a shader RED.
The corrected variant derives a target from the actual ballot mask, shuffles
that active lane's `LaneId`, and passes 96/120000 native readback plus large
GPUAV. Neighboring 753-image, loop/buffer and wide SPIR-V cases pass on the
final test exe. No production GPU change or game retry followed these GREEN
controls. Menu and gameplay remain unverified.

The captured game VS SPIR-V has one loop, 250 `OpSampledImage` instructions,
13 buffers, and a shuffle target first masked to 0–63 and then to 0–31 by
the existing graphics wave64 partition helper. That helper is documented
below as a limited native32 approximation, not full cross-subgroup wave64
equivalence. Static SPIR-V cannot prove which target lanes or loop iterations
the game executed. A semantic fix needs an independent valid guest-wave64
regression and a feasible host execution model; truncating targets or forcing
a loop budget does not establish correctness.

## Windows SysV host-entry stack alignment (2026-10-03)

Upstream PR #990 head `617e728c9309337fda2da0aba9f31bed0207930f`
reports that a guest-to-host SysV call on Windows can enter with RSP at 0
mod 16, then make an MS ABI call with invalid alignment. This is a generic
Windows ABI bug with a synthetic upstream test; the newest Yōtei blocker is
GPU DeviceLost, so do not attribute it to this fault. Before any local ABI
change, port only the trampoline test into `virtual_memory_allocation_tests`,
build natively, and prove the unmodified macro fails on the expected
`ms_entry_rsp` assertion. Then update the shared ABI macro and rerun the
unchanged test, neighboring red-zone/fiber/memory cases and the native build.
Keep Windows x64 and other platforms distinct. A host ABI test cannot prove
menu or gameplay.

Local proof on branch `yotei-windows-bringup`: ported only the upstream
trampoline test, built with `_Build/windows-local.cmd build-target
virtual_memory_allocation_tests`, then ran `--sysv-align-only` on the old
macro. It failed as intended with exit 1 and
`SysV host entry propagated a misaligned stack into an MS ABI call`
(`_Build/logs/pr990-sysv-align-red-20261003.log*`, test exe SHA-256
`1ddcb1f991984d4e3d11286621c10bf39ef8c08c1ebe4e83eb271325a02062e4`).
Commit `79db93d6` adds `force_align_arg_pointer` only for Windows
x64. The unchanged test passes with `entry_mod16=0` and `ms_mod16=8`; focused
fiber, rsqrt and red-zone tests and the complete
`virtual_memory_allocation_tests` executable all passed (logs
`_Build/logs/pr990-*-green-20261003.log*`). Native emulator build/install
passed; installed exe SHA-256
`5251cfc12f986dda2e9b3c8c761e0e80b5fe0545493dcc210cad803c2da406da`.
No game retry on this exe yet; the known repeated GPU DeviceLost is unrelated
to the synthetic ABI failure.

## DeviceLost on large indirect-image vertex draw (2026-10-03)

Clean-source retry on commit `22211827`, installed exe SHA-256
`8c33160e9bd0d89d67f4d9d9357a7ae2b11b02bc4437d55118fcbf448a703d32`:
`_Build/runs/yotei-integrated-20261003-181855-presentfix-gpuav` finalized
`run.json` with exit 321, `shown=200`, 18m22s elapsed. Prepared readback
was RGB black through frame 197, then `colored=10` at frame 198 and
`colored=62` at frame 199. `vkWaitSemaphores` returned `ErrorDeviceLost` at
wait tick 41333, known GPU tick 41331. No validation error preceded it.
This corroborates the old frontier but did not use per-draw SyncDiag, so
the exact submitted operation in this run remains unproved. The clean
retry is not evidence that the diagnostic formatted-descriptor failure
has been fixed or reached. Avoid repeating the driver reset until a
bounded synthetic Vertex regression isolates the cause.

Native indexed-draw control added in test-only commit `0201fa00` after the
full compute switch passed. It extends the existing Vulkan graphics harness
with an optional explicit uint32 index buffer, draws a repeated public
triangle and compares the same one-pixel output as the neighboring ordinary
`GraphicsPositionWExport` case. Counts 3 and 120000 both pass; the large
case also passes bundled-layer GPU assisted validation without a validation
error. Test exe SHA-256
`b42fd19cdd79c1dc874433123c2961cddad311257a1f6401b78730756ca844bd`;
native build `_Build/logs/indexed-control-final-build-20261003.log`, runs
`_Build/logs/indexed-{neighbor,small,large,large-gpuav}-final-20261003.log*`
all exit 0 without timeout. 120000 is a round synthetic bound above the
observed 119856 game indices. The control uses a simple passthrough VS,
so it does **not** execute the game's image table, vertex inputs, pipeline
bindings or descriptor state. It narrows the next test to their combination;
it is not a RED and does not justify a production workaround.

The same native exe SHA-256 `4b93dd3d2dd2c7964dbbb30446bcdad21506eba1e792a56405bfd479bae9ebf6`
gave two separate GPUAV+shader-instrumentation runs with nonzero loading
spinner pixels, then `vkWaitSemaphores` `ErrorDeviceLost`: shown 203 / tick
41560 in `_Build/runs/yotei-integrated-20261002-212649-presentprobe-gpuav`,
and shown 381 / tick 54581 in
`_Build/runs/yotei-integrated-20261003-075610-presentprobe-gpuav`. The
second run exited 321 by itself; its `run.json` is finalized. Windows System
log has `nvlddmkm` event 153 at the second loss. No validation error or
stderr preceded the loss. Its last CPU shader log is VS
`e3125617f3efc38f` with 251 image resources / 250 sampled pairs. Earlier
SyncDiag evidence (see section below) put the GPU wait on the first indexed
draw with this VS, 119856 indices, and a specific PS. That is a strong lead,
but the new runs did not enable per-draw SyncDiag, so their last submitted GPU
operation is not independently identified.

Required synthetic regression before an emitter/descriptor fix: construct a
public vertex fixture with an indirect image table large enough to exercise
the same mechanism, without copying a game shader. Prove SPIR-V validation
and selected image values for first, middle, last and missing keys with
distinct texture contents, including sparse table entries. Bound a native
indexed draw at increasing table and index counts, capture GPU output and
synchronization result, and find the smallest failing combination without
repeated driver resets. Compare shader instrumentation on/off only after a
safe isolated probe; verify descriptor-indexing/device capability requirements
and preserve heterogeneous-image rejection. A pass of CPU validation or a
pipeline build is not a draw result. Do not change the 250-way selection to
an opaque `OpSampledImage` Phi; that prior variant was invalid. Until the
failing mechanism is reproduced, no production workaround is justified.

Synthetic progress at test commit `12baacd3`, native test exe SHA-256
`92ba731d43692969b360ec60cc4bc850ec3e694048191ceeeb853439aecf866d`:
`--wide-indirect-image-spirv-only` builds an independent IR image table with
251 typed choices (root plus 250 cases); SPIR-V validates and retains one
`OpSwitch` and 251 sample operations. `--wide-indirect-image-table-compile-only`
and `--wide-indirect-image-table-gpu-only` use a guest compute fixture with
250 distinct 4×4 textures; CPU-visible indices 0, 125, 249 are correctly
pruned to three pairs, and a bounded GPU invocation returns their distinct
expected values. All three exit 0; neighboring `--indirect-image-only`,
`--indirect-image-numeric-only` and
`--homogeneous-indirect-image-validation-only` also exit 0. Logs are
`_Build/logs/wide-indirect-final-*-20261003.log*` and
`_Build/logs/wide-indirect-table-*-20261003.log*`. An earlier material-stride
variant was discarded: it scanned 27233 speculative positions and hit the
separate pair limit 513 before shader execution, so it was not a valid RED.

These passing checks narrow the problem: a legal 250-case switch and three
actual texture selections work independently. They do not execute a
250-candidate switch on the GPU or a large vertex/indexed draw. Next use a
GPU-owned selector to keep the full table live, establish a small numerical
GPU control, then increase only within a safe bound to find a failing draw
without repeated driver resets. Missing/sparse keys remain uncovered.

Follow-up test-only commit `38b4fa6c` extends the same coherent guest fixture:
the index list can contain 16 or all 250 distinct keys, so materialization
retains 16 or 250 live sampled pairs and SPIR-V has the matching number of
sample operations. The full case executes one bounded compute dispatch and
compares **all 250** independently assigned floating-point texture values.
Native test exe SHA-256
`cb502a5715ca0048d7c792f77c973c01cdfbd66d32fa21775c221012270df6a0`.
`_Build/logs/wide-indirect-full-compile-20261003.log*`,
`wide-indirect-16-gpu-20261003.log*`,
`wide-indirect-full-gpu-20261003.log*`,
`wide-indirect-16-proper-gpuav-20261003.log*`, and
`wide-indirect-full-proper-gpuav-20261003.log*` all have `exitCode=0`,
`timedOut=false`. The previous three-choice GPU case also passes unchanged
(`wide-indirect-three-recheck-20261003.log*`). The GPUAV run used
`_Build/run-regression-gpuav.ps1` and its bundled validation layer; a first
attempt with `KYTY_TEST_GPU_ASSISTED_VALIDATION=1` but no `VK_LAYER_PATH`
failed at `vkCreateInstance ErrorLayerNotPresent` and is not a shader RED
(`wide-indirect-full-gpuav-20261003.log*`). No driver reset occurred.

This covers the full 250-way **compute** switch on the current GPU, including
first, middle, and last keys, but not a vertex shader or indexed draw. The
GPU selector is read from a coherent CPU-visible guest buffer; GPU-owned
selector semantics and missing/sparse keys remain unproved. The next required
synthetic regression is a bounded native vertex fixture with explicit image
descriptors, a small indexed draw control and progressive index/resource
counts, GPU readback, and GPUAV. Keep the first actual failure as the RED;
do not infer that this passing compute test fixes the game DeviceLost.

A diagnostic attempt to hide only the CPU index words from the clean
specialization reader did **not** produce such a control. Materialization
correctly rejected `bounded SRT read 0 index 0 cannot read coherent source at
0x40` (`_Build/logs/wide-indirect-unknown-compile-20261003.log.stderr`).
The attempted harness change was removed; it was not a RED for the draw and
no GPU run was made. The next fixture must either provide a genuinely
coherent producer/consumer contract for that guest scalar read, or execute
the already validated synthetic IR module with a GPU-generated key and
explicit Vulkan descriptors. Preserve the refusal for unreadable planning
inputs.

## Wave-wide zero branches with inactive lanes (2026-10-02)

Upstream PR #985 head `139a260e4975dd612cfc4e84d84c002bfa9c2b39`
describes a shared shader-emitter defect: a `*Zero` scalar branch compares
the ballot of the condition with all ones, although inactive lanes do not
vote. A partial final wave can therefore remain in a loop. This is a lead
for the current Yōtei stall near shown 156–163, not a proven attribution.
Required synthetic RED: compile a small `s_cbranch_execz` loop for a guest
wave64 split into host wave32 and verify its SPIR-V tests that no *active*
lane fails; compare `s_cbranch_execnz` and existing neighboring branch tests.
Then make the shared emitter correction, run the unchanged test GREEN and
neighboring branch/ballot cases, and retry the bounded native game. A loop
cap as in PR #986 is a separate safety bound and cannot replace correct
branch semantics or prove a visible frame.

The initial opcode-count test passed before the fix and was too weak. The
strengthened test identified `OpIEqual` against a `0xffffffff` constant in
the emitted ExecZero loop and failed with exit 9
(`_Build/logs/zero-branch-structural-red-20261002.*`). The corrected emitter
ballots the inverted predicate and tests for zero; the unchanged test passed
(`_Build/logs/zero-branch-green-20261002.*`). Targeted
`--wave64-condition-ref-only`, graphics collective routing and scalar-mask
branch checks passed. `--single-wave64-ballot-spirv-only` still fails on a
separate native subgroup ballot expectation; its ordinary ballot path was
unchanged by this correction, so the full CFG suite is not yet green.
The final test source was rebuilt with `_Build/windows-local.cmd build-target
shader_cfg_tests`; `--zero-branch-ballot-only` and those three neighboring
cases passed again in `_Build/logs/zero-branch-final-*-20261003.log.run.json`
using test exe SHA-256 `295e981a8e1a91dc5c00103f1e0c3473d984f85ee63a22dfe4e598f3fb05b40c`.
The code and test are committed as `37b35115`.

Later inspection of the Yōtei run narrowed the visible stall more directly:
the last logged call before shown stopped at 156 is
`vkCreateComputePipelines` for CS `54904fb419d79e49`, after an emitted
771986-word SPIR-V (`_Build/runs/yotei-integrated-20261002-205909-presentprobe-gpuav`,
`_kyty.txt:7609520-7609525`). No completion line follows before the 240 s
watchdog. This establishes a pipeline-creation stall, not a proven runtime
loop in that shader. Assess the large binary and the driver's compiler
separately; do not attribute the stall to #985 without a native retry.

The first bounded native retry with this correction, exe SHA-256
`4b93dd3d2dd2c7964dbbb30446bcdad21506eba1e792a56405bfd479bae9ebf6`,
eventually compiled that 773030-word shader (`vkCreateComputePipelines`
`elapsed_ms=287593`) and other huge variants. Source readback in
`_Build/runs/yotei-integrated-20261002-212649-presentprobe-gpuav` proved
the first current-branch RGB pixels at frame 201 (`colored=10`) and reached
frame 203 (`colored=92`); then the device returned `ErrorDeviceLost` at GPU
tick 41560. The launching monitor ended before it could finalize `run.json`,
so use the log/readback rather than its stale `stopReason`/`maxShown` fields.
This proves that the earlier 240 s watchdog was too short for cold GPUAV
pipeline creation; it does not prove #985 caused the color progress or that a
menu was rendered. The separately bounded warm retry's final result is
recorded in the DeviceLost section above.

## Driver compiler crash without GPUAV (2026-10-02)

Next synthetic regression (2026-10-03): construct the same
`vk::ComputePipelineCreateInfo` that `CreatePipelineInternal` submits, using
`CompiledShaderInfo::compute_cooperative_wave64=true`, and assert that its
`flags` contains `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`. Check the
ordinary compute case remains unflagged. The existing policy-only helper
test passes even though the submitted create info currently has zero flags;
record a native RED before changing production behavior. This is a pipeline
configuration regression, not a standalone reproduction of the NVIDIA crash.

Native RED/GREEN proof: `_Build/windows-local.cmd build-target shader_cfg_tests`
and `--cooperative-pipeline-flags-only` on test exe SHA-256
`07d1185f42c3e9dabb881abf9a208409429b012b0e6a04b6b28d6496aca9a0d5`
failed the intended flag assertion (`_Build/logs/compute-pipeline-flags-red-cfg-20261003.txt*`).
The same test on exe SHA-256
`63db5e9003d73e30b05a1fea4077aaa519ffb59dc161fb6f223e20442e24026f`
passes after routing `CompiledShaderInfo` metadata through the actual
`vk::ComputePipelineCreateInfo` builder. Neighboring
`--spirv-optimization-only` and `--cooperative-wave64-admission-only` pass.
Native emulator build/install passed; installed exe SHA-256
`be1dbd195b9b9a0ca6454ac2ed77835cd7a08442c27903cb90549a9559cb3623`.
Bounded game retry `_Build/runs/yotei-integrated-20261003-103053-presentfix-gpuav`
with GPUAV shader instrumentation disabled stopped again at `shown=0`,
`exitCode=-2147483645` during the same `b90e2024732c6111`
`vkCreateComputePipelines`. New trace proves this shader has
`compute_cooperative_wave64=0` and `flags=0x0`, so the validated pipeline
flag correction does not address the game compiler crash. Do not repeat this
mode without a separate compiler/layout reproducer or a relevant shader fix.

Isolated compiler reproduction on 2026-10-03: the saved, validator-clean
39173-word module from `_Build/runs/yotei-integrated-20261003-090128-presentprobe-gpuav`
also exits `0x80000003` in a bounded native Vulkan probe using the module's
seven descriptor bindings (including array counts), one push-descriptor set,
and 128-byte compute push-constant range. The disposable probe source and
binary are `_Build/analysis/vk_spv_probe-layout-20261003.cpp` and
`_Build/windows/vk_spv_probe_layout.exe` (SHA-256
`36563888bd5b96eddd335c37d0dee8532523c2c0bdfb03a10ccde92b6b685c85`).
Probe build used a temporary CMake target and `_Build/windows-local.cmd`;
the temporary CMake change was removed. Logs:
`_Build/logs/vk-spv-b90e-layout-probe-20261003.txt*` and
`vk-spv-b90e-layout-noopt-20261003.txt*`. Disabling driver pipeline
optimization still crashes. The probe does not model all production device
state, but it reproduces the same pipeline creation boundary without game
execution, command submission or a GPU watchdog.

Diagnostic copies under `_Build/analysis/b90e-layout-20261003` all pass
`spirv-val --target-env vulkan1.3`. Replacing all storage-buffer loads, or
just the first load from `bda_pagetable` in `get_bda_pointer`, with `OpUndef`
allows pipeline creation. That is **not** a valid fix: it removes guest
address translation. Replacing all calls to that helper and eliminating the
unused helper still crashes; therefore the individual load is not an
established unique cause. `Volatile`, explicit alignment, `OpAtomicLoad`,
two 32-bit components, constant page index, select instead of return Phi,
SPIR-V `-O`, and exhaustive inlining leave the compiler crash intact.
Artifacts are `vk-spv-b90e-*-20261003.txt*`; no transformed SPIR-V was
installed in the emulator. Before a production change, build a public
synthetic module with the relevant storage-buffer/BDA and control-flow
shape that reproduces the compiler fault or an independently wrong guest
result, then prove RED/GREEN without zeroing memory or suppressing the fault.

New bounded retry on 2026-10-03 with GPUAV enabled but shader
instrumentation disabled used installed emulator exe SHA-256
`5251cfc12f986dda2e9b3c8c761e0e80b5fe0545493dcc210cad803c2da406da`.
`_Build/runs/yotei-integrated-20261003-090128-presentprobe-gpuav/run.json`
finalized with `process-exit`, `exitCode=-2147483645` (`0x80000003`),
`maxShown=0`, `coloredProven=false`. `_kyty.txt` ends at
`vkCreateComputePipelines begin` for CS `b90e2024732c6111`; Windows
Application Error 1000 again names NVIDIA `nvgpucomp64.dll` version
`32.0.16.1714`, offset `0x589eb2`. The two new captured SPIR-V copies are
identical (SHA-256
`53dbb68a1f5886a99b1009df507931f4134dab806ef0c8f4843ea4572690092c`),
but differ from the 2026-10-02 module (SHA-256
`0900d7d8fc9da9acf8b9ba47c1b25b2bbedc9704a9fc45bfd57a44e3c4ec99c9`).
Their instruction histograms differ by 11 each of SPIR-V opcodes 168, 197
and 199; the compiler fault persists. This run is not a vertex-draw reproduction or
evidence that the Windows SysV ABI fix changed GPU behavior. Before another
uninstrumented game retry, isolate a failing generic compiler pattern in a
bounded standalone probe with a layout matching the production pipeline;
the older probe's layout did not reproduce game behavior reliably.

The bounded current-branch run
`_Build/runs/yotei-integrated-20261002-204937-presentprobe-noval`
used 2560x1440, no Vulkan core validation and no GPUAV. It crashed before
`shown=1` with Windows exception `0x80000003` in NVIDIA
`nvgpucomp64.dll` version `32.0.16.1714` while creating the compute pipeline
for shader `b90e2024732c6111`; WER Application Error event 1000 records
offset `0x589eb2`. The two identical emitted SPIR-V copies are in that run's
`shaders/0070_...spv` and `0071_...spv`. This is a driver compiler boundary,
not evidence of a visible frame or menu. Avoid repeated uninstrumented game
launches until a bounded synthetic SPIR-V or emitter regression isolates the
instruction pattern that triggers the crash. Keep shader validation and GPUAV
controls to distinguish invalid SPIR-V from a driver defect.

## Prepared-frame readback layout after a guest flip (2026-10-03)

The bounded full-GPUAV retry
`_Build/runs/yotei-integrated-20261003-163915-presentfix-gpuav` reached
`shown=150` and then failed Vulkan validation on the first requested
prepared-frame readback: `copyImageToBuffer` required
`VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, but the image remained in
`VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`. `Presenter::Frame::CopyFrom` leaves
the prepared image in transfer-destination state, while
`CapturePreparedFrame` copies from it without a transition.

Required synthetic regression: create a small native prepared-format image,
clear it in transfer-destination layout, call the same readback recorder used
by presentation, and require both transfer-source state and the numerical
clear color in the downloaded buffer. Run RED against the existing recorder,
then GREEN after adding the source transition. Retain the actual game retry
as a separate runtime check.

The native `--prepared-frame-readback-only` fixture used a 2×2 RGBA8 image
cleared to `(0.25, 0.5, 1, 1)`. With the old recorder it failed after a
successful build at `source layout before readback` (exit `0xC0000409`,
`_Build/logs/prepared-frame-readback-red-20261003.stderr`). After routing
the recorder through the prepared-image transition, the unchanged test
passed with numerical pixels and a second readback from an already-source
image (`_Build/logs/prepared-frame-readback-green-20261003.run.json` and
`_Build/logs/prepared-frame-readback-neighbor-20261003.run.json`). The bounded
game retry `_Build/runs/yotei-integrated-20261003-170145-presentfix-gpuav`
captured 16 prepared frames, 120–135, without the old layout error; every
captured frame had zero RGB. It later reached `shown=197` and the separate
16-bit SRGB color-target guard. These readbacks prove the layout fix and
black pixels for that interval only; they do not prove a menu.

## 8-bit SRGB render-target element size after expanded HTile alias (2026-10-03)

The bounded GPUAV run
`_Build/runs/yotei-integrated-20261003-161215-presentfix-gpuav-sync`
passed the previous HTile guard and exited 321 at `shown=192` with
`unsupported render-target format combination: layout=1 type=6 order=0`
in `textureCommon.cpp:138`. The initial interpretation as 16-bit was wrong:
the current `ChannelLayout` enum assigns `1` to `k8`; `6` is `kSrgb`.
The bounded diagnostic run
`_Build/runs/yotei-integrated-20261003-174110-presentfix-gpuav` proved
`origin=RefreshShaders valid=1 buffer=128 components=1`, i.e. the valid
guest encoding is `k8Srgb`. The host mapping is R8 UNorm, but the format
table gives its render-target element size as zero, so the tuple is
rejected. Upstream PR #1003 changes this element size from 0 to 1 and
does the corresponding 8+8 SRGB correction from 0 to 2. No readback was
requested in this particular run; prior bounded runs measured prepared
RGB black on frames 120–135 and 180–195.

Required synthetic RED before the production change: request the actual
`k8/kSrgb/standard` render-target tuple through
`TextureGetRenderTargetFormat`, require one byte and R8 UNorm backing, and
also cover `k8_8/kSrgb` with two bytes and R8G8 UNorm. Retain a negative
unsupported 16-bit SRGB encoding. Run the same test unchanged GREEN after
correcting shared format metadata, then retry the game.

Completed native RED/GREEN: `--reverse-rt-only` built successfully, then
failed with exit 321 at the exact `k8/kSrgb/standard` tuple before the
metadata change (`_Build/logs/srgb-rt-red-20261003.run.json`). The unchanged
test passed after applying the two element sizes from upstream PR #1003
(`_Build/logs/srgb-rt-green-20261003.run.json`); its 16-bit SRGB negative
encoding remains invalid. Neighboring tile tests initially exposed stale
sampled-only assumptions; the adjusted assertions preserve `k9_9_9_5Float`
as non-target and check exact BPE for both SRGB layouts. Final native
`--rt-tiled-sampled-format-only` passed and `--gpu-tiler-only` passed 330
cases / 236 format-mode pairs (`_Build/logs/srgb-rt-tiled-final-20261003.run.json`,
`_Build/logs/srgb-rt-gpu-tiler-final-20261003.run.json`). Game retry is
still required before claiming this reaches a spinner or menu.

## Generic image lookup after a sampled HTile clear import (2026-10-03)

The bounded GPUAV run
`_Build/runs/yotei-integrated-20261003-141139-presentfix-gpuav-sync`
stopped at `shown=195` in `TextureCache::FindImage`:
`sampled HTile import requires its metadata-aware lookup path`. This guard
sees an overlap with a previously imported virtual depth clear, but the log
does not identify whether the new request is a sampled texture, storage image,
color target, or depth target. The temporary Vertex loop cap was still active;
the run had no pixel readback and does not prove a visible frame.

The diagnostic retry
`_Build/runs/yotei-integrated-20261003-143757-presentfix-gpuav-sync`
identified an exact `DepthTarget` rebinding: both owner and request used
data `0x5053070000+0x1000000`, HTile `0x5054070000+0x40000`, D32 format,
and 2048×2048, one layer. It exited 321 at `shown=192`; the counter is not
pixel proof. The diagnostic patch was reverted after capture.

The independent native `--sampled-htile-depth-promotion-only` fixture uses
uniform HTile clear1 over stale raw depth, numerically samples the logical
depth, binds the exact same allocation as a native depth target, clears it to
zero, and numerically samples the new pixels. It was RED at the original
`FindImage` guard (`_Build/logs/htile-depth-promotion-red2-20261003.*`) and
GREEN with exact-owner promotion
(`_Build/logs/htile-depth-promotion-final-green-20261003.*`). A different
HTile range remains rejected with exit 321
(`_Build/logs/htile-depth-mismatch-final-20261003.*`). Neighboring array
clear and native HTile subset fixtures pass
(`_Build/logs/htile-array-neighbor-20261003.*`,
`_Build/logs/native-htile-subset-neighbor-20261003.*`). The first test design
using the older clear-discovery fixture stopped before the transition at its
unrelated pattern-fill setup and is not counted as RED. Game retry is pending;
this test does not prove a menu.

Two later bounded game retries did not verify the exact-owner path. The first
(`_Build/runs/yotei-integrated-20261003-150014-presentfix-gpuav-sync`)
closed by a 900-second shown-frame watchdog at `shown=153` while compiling
instrumented pipelines. The warm retry
(`_Build/runs/yotei-integrated-20261003-152203-presentfix-gpuav-sync`)
reached `shown=193` and hit the same guard. Detailed diagnosis in
`_Build/runs/yotei-integrated-20261003-154734-presentfix-gpuav-sync` shows
an overlapping depth target with pitch 4096 and a 64 MiB mip footprint,
versus the imported owner's pitch 2048 and 16 MiB footprint; its HTile range
also differs. This is a different allocation layout, not the exact-owner
transition proved above. Required RED before changing it: a synthetic
metadata-aware imported clear followed by a larger overlapping depth target,
with numerical proof that the target sees the correct guest memory layout and
no stale raw depth bytes. Keep the mismatch guard until such a transfer is
proven; gather the full request/owner ranges and clear/load state first.
The full-range retry
`_Build/runs/yotei-integrated-20261003-155620-presentfix-gpuav-sync`
confirmed the alias precisely: both start at `0x50540b0000`; owner data is
16 MiB at 2048×2048, target data 64 MiB at 4096×4096; both HTile ranges
start at `0x50580b0000`, but owner uses 256 KiB and target 1 MiB. Neither
source has CPU- or buffer-dirty pixels at the guard. This is a coherent
native clear that must be materialized into the overlapping guest layout
before the larger target can acquire it; blindly accepting the owner would
use the wrong pitch and extent.
The independent native `--sampled-htile-expanded-alias-only` case now
materializes a 512×512 HTile clear over stale raw bytes, then acquires a
1024×1024 depth target at the same data/metadata bases with larger footprints.
Its two numerical probes require the first target texel to retain clear1 and
the far tail to retain raw 0.25. It is RED at the existing guard, after the
first logical clear was sampled successfully
(`_Build/logs/htile-expanded-alias-red-20261003.*`). Required GREEN: coherent
download of the imported native depth to guest backing, retirement of that
owner, then ordinary larger-target upload without losing metadata ownership.
The shared texture cache now performs that transfer only for a clean,
single-layer D32 depth owner and a larger attachment at the same data and
HTile bases with compatible format/layout identity. It flushes and waits for
the guest backing write before retiring the old owner. The unchanged
numerical alias fixture is GREEN
(`_Build/logs/htile-expanded-alias-final-green-20261003.*`); a different
HTile base still fails closed with exit 321
(`_Build/logs/htile-expanded-mismatch-final-20261003.*`). The exact-owner
and array-clear neighbors pass after this change
(`_Build/logs/htile-exact-neighbor-after-expanded-20261003.*`,
`_Build/logs/htile-array-neighbor-after-expanded-20261003.*`). The bounded
game retry passed the old HTile guard and reached the 16-bit SRGB format
blocker described above. The retry did not numerically check the real
2048→4096 transfer or prove visible pixels.

## GPU-produced indirect workgroup bounds for bounded SRT (2026-10-03)

The bounded GPUAV run `_Build/runs/yotei-integrated-20261003-125433-presentfix-gpuav-sync`
exited 321 after `shown=193`: compute shader `a2dfc83fbe2e55b8` uses a
workgroup-indexed bounded SRT snapshot, while its indirect dispatch arguments
are GPU-owned. The CPU copy can be stale, and `MaterializeResources` correctly
rejects untrusted workgroup counts. The prior temporary Vertex cap was a
diagnostic and is not a fix.

The synthetic native `--gpu-owned-bounded-indirect-only` case now writes a
`2×1×1` grid through the GPU while the CPU copy remains zero, then requests a
shader whose scalar coefficient table is indexed by `WorkgroupId.x`. The
final test was RED with only the production patch reversed: exit 321 on
`bounded SRT requires coherent indirect workgroup counts`
(`_Build/logs/gpu-owned-bounded-indirect-final-red-20261003.*`). It is GREEN
with the production patch: the coherent CPU copy becomes `2×1×1` and the
materializer/pipeline accepts the shader
(`_Build/logs/gpu-owned-bounded-indirect-final-green-20261003.*`). The
ordinary empty indirect control also passes
(`_Build/logs/empty-indirect-after-coherent-grid-20261003.*`). The shared
pipeline cache drains GPU-owned argument bytes only when the shader plan
needs workgroup-indexed bounded SRT; a failed coherent read retains the
materializer's fail-closed error.

This regression proves host-side count coherence and shader admission, not
GPU execution. Its tiny storage-output shader produced no numerical output
even under a direct dispatch in diagnostic trials; those attempts are in
`_Build/logs/gpu-owned-bounded-indirect-green5-20261003.*` and later logs.
Required follow-up: an independent, numerically checked native GPU fixture
with a working direct control; zero-axis, changed-grid and unreadable
GPU-owned argument variations; then rerun the game. Do not report this
shader as a successful rendered-output test.

## Zero-sized indirect compute dispatch (2026-10-02)

The bounded GPUAV run
`_Build/runs/yotei-integrated-20261002-200407-presentprobe-gpuav`
exited 321 at shown 125 on the same immutable-SRT/writable-buffer alias.
Temporary diagnostics show the failing specialization has guest grid
`{0,1,1}`. Its earlier nonempty grids `{4096,1,1}`, `{727,1,1}`,
`{64,1,1}` and `{320,1,1}` evaluated their selectors and pruned the
unselected overlapping row. Source readback frames 60–125 remain RGB black.

Required RED: an indirect compute dispatch with a zero group count and an
unreadable shader address must be skipped before shader specialization, as
the existing direct-dispatch control does. Cover each zero axis and a
nonzero control. Before treating CPU-visible indirect args as authoritative,
check GPU-dirty ownership and pending producer writes: the recorded Vulkan
dispatch reads the argument buffer later, so a stale CPU zero cannot justify
skipping a possible GPU-generated nonzero dispatch. Preserve the alias guard
for any unproved case. Repeat the native game run after a generic fix.

The isolated native `--empty-indirect-only` test (all three zero axes,
unreadable shader address) was RED on the old path: `ShaderGetInputInfoCS()`
tried shader address 1 (`_Build/logs/empty-indirect-isolated-red-20261002.*`).
It passed unchanged after the renderer skipped only CPU-owned zero grids
(`_Build/logs/empty-indirect-isolated-green-20261002.*`). The existing
`--native-indirect-only` GPU-produced argument test still fails at its
asynchronous host-submission assertion (exit 9); its numerical controls
cannot be counted as passed. Do not weaken that assertion merely to make the
new shortcut look safe.

Required additional RED: a bounded SRT selector with an apparent small CPU
grid that would prune an overlapping writer, while the indirect argument
range is marked GPU-owned. Materialization must refuse the proof because the
GPU may execute a larger grid; the same exact CPU-owned grid remains the
positive control. This prevents the new dispatch-wide pruning from treating
stale indirect counts as authoritative.

That `--unselected-bounded-writer-only` extension was RED on the old
materializer (`_Build/logs/untrusted-grid-red-20261002.*`) and GREEN once
GPU-owned counts were marked untrusted and dispatch-wide pruning refused
them (`_Build/logs/untrusted-grid-green-20261002.*`). Full
`resource_tracking_tests` and `resource_materialization_tests` pass.
The final native GPUAV run on source `36bcf354`, exe SHA-256
`567600051cfd44a3a71ea9f126614abb52ebfc3058a2547aeb8ccab894fa8b80`,
`_Build/runs/yotei-integrated-20261002-205909-presentprobe-gpuav`, reached
shown 156 past the old fatal and then stopped by the frame watchdog; source
RGB frames 130–155 were all black. The next runtime stall remains unproved.

## Unselected bounded buffer writer (2026-10-02)

The bounded GPUAV diagnostic run
`_Build/runs/yotei-integrated-20261002-185718-presentprobe-gpuav`
reaches CS `34e090c623ad611c` at shown 142. Its 32-row buffer table has an
overlapping writable descriptor in row 17, but all 64 launched workgroups
read selector words with bits 16–20 equal to zero from a separate scalar
input buffer. The shader therefore selects row 0 in this captured dispatch;
the row-17 writer is only an enumerated candidate. This is a lead, not yet a
safe exception: the selector input must be snapshotted coherently and protected
from every possible shader write.

Required synthetic RED: a compute bounded buffer table with a dynamic
workgroup-uniform selector read from a scalar input buffer, one disjoint
selected writer and one overlapping unselected writer. The current admission
must reject the dispatch. GREEN may admit it only after enumerating every
workgroup selector, retaining its exact input read ranges, proving those
ranges cannot be written, and disabling only unreachable candidates for this
snapshot. Controls must preserve rejection when any workgroup selects the
overlapping row, when the selector cannot be evaluated or read coherently,
and when another live writer can mutate the selector input. Verify renderer
binding admission and repeat the bounded native game run; a black readback
does not prove a menu.

The first synthetic `--unselected-bounded-writer-only` reproduced the expected
CPU rejection on the old code and passed after a generic workgroup-selector
evaluation/pruning change. Its controls preserve rejection for a selected
overlap, unreadable selector input and a writer of the selector input. Full
`resource_tracking_tests` and `resource_materialization_tests` passed.
However, native exe SHA-256
`13b9913741771a10289d75183541afb68af0d46c8c96769a89dadb001cb59e65`
in `_Build/runs/yotei-integrated-20261002-192412-presentprobe-gpuav`
still exited 321 at shown 137 on the same `0x5000f37f80+120` alias
(dense resource 9). Readback frames 60–136 remained black. Later diagnostics
below identified the zero-sized indirect dispatch; the selector itself was
evaluated successfully for nonempty grids.

The bounded diagnostic rerun
`_Build/runs/yotei-integrated-20261002-193319-presentprobe-gpuav`
on a diagnostic build again exited 321 (shown 133). Earlier dispatches of
the same shader proved row 0 or 1 active; immediately before the fatal alias,
the reachability helper returned before visiting the 32-row table. The
initial hypothesis was the combined workgroup-count cap. A follow-up RED
used a synthetic selector
dependent only on WorkgroupId.x with a large independent y dimension whose
total grid exceeds 65536; the old helper rejected despite four relevant x
values. GREEN discovers every WorkgroupId axis transitively used by the
selector, including nested SRT reads, and enumerates only that Cartesian
domain. The unchanged x and y tests pass. Later game diagnostics measured
`{0,1,1}` for the failing dispatch, disproving the count-cap hypothesis as
its cause; the zero-sized indirect case above is the actual next blocker.

## Prepared-frame readback layout transition (2026-10-02)

Bounded native retry `_Build/runs/yotei-integrated-20261002-180746-presentprobe-gpuav`
on installed exe SHA-256 `4c92009ed90e9a6a41309c4fb45eff1ae6f6af98f85570d1d6a6832efd742919`
set `ReadbackStart=90` and exited 321 at shown 86, with no readback file or
pixel proof. The fatal validation message in `_kyty.txt` reports that a
`vkQueueSubmit` copy expects a prepared image in `TRANSFER_SRC_OPTIMAL` while
its current layout is `TRANSFER_DST_OPTIMAL`. This is on the optional
presentation readback path, before the later immutable-SRT alias seen in a
run with `ReadbackStart=180`.

`Presenter::Frame::CopyFrom` leaves the frame in transfer-destination layout;
`CapturePreparedFrame` submits `copyImageToBuffer` with transfer-source layout
without a transition. Required synthetic RED: exercise the prepared-frame
copy followed by optional capture with Vulkan validation and assert the
image layout/transfer access is correct at submission. Cover guest-source
capture and ordinary presentation when capture is disabled. Only then correct
the shared presentation readback transition and rerun the same test and a
bounded game readback. Do not infer visible pixels from `shown`.

The existing guest-source readback option is a diagnostic workaround for this
prepared-frame path. A copy of the bounded runner under `_Build` enabled it
without changing production code. Run
`_Build/runs/yotei-integrated-20261002-181225-presentprobe-gpuav` reached
the same live SRT alias at shown 133; `present-readback.txt` contains 74
frames 60–133, all `colored=0` with RGB min/max zero. The prepared-frame
layout issue remains pending despite this valid source readback.

## Immutable SRT source aliases a writable buffer (2026-10-02)

Native retry `_Build/runs/yotei-integrated-20261002-161240-menucheck-gpuav-sync`
on the unsigned-sampler correction exits 321 at shown 135. CS
`0x34e090c623ad611c` rejects a pre-dispatch immutable SRT snapshot
`0x201347e200+512`: a writable buffer candidate starts at
`0x201347e390`, descriptor size 1, contiguous mapped prefix 1, and lies
inside that source. Its origin is 3, first use PC `0x1d94`, zero stride,
`max_byte_extent=16`, formatted descriptor-only access. The renderer has an
explicit dispatch-wide alias guard; current resource tracking does not prove
read-before-write across invocations. Do not bypass either guard using only
local CFG order. The one-byte mapped prefix alone did not establish that a
four-byte store was impossible: the host binding rounds the range for DWORD
storage. The guest descriptor's mode-0/zero-stride bounds do establish it.

Required synthetic RED for a **live** overlap: a compute shader with a bounded
SRT read and a potentially overlapping writable candidate in distinct
invocations, plus an independent disjoint candidate and exact boundary
cases. Establish guest ordering/visibility semantics and verify numerical
GPU results, including write-first/read-first schedules, before choosing a
shared mechanism such as a coherent runtime read. Preserve rejection for
unproved cross-invocation aliasing.

Follow-up diagnosis: the captured writer descriptor has `OOB_SELECT=0` and
`STRIDE=0`. The AMD RDNA2 ISA specifies mode 0 as out of bounds when
`offset >= STRIDE` and drops out-of-bounds writes. Existing specialization
marked this exact combination `zero_stride_oob` but previously applied it only
to vector reads. Narrower regression: numerical native GPU test with a
mode-0/stride-zero store aimed inside a sentinel, plus a mode-3/stride-zero
control store that must write; CPU bounded-SRT snapshot test for an overlapping
mode-0 candidate and rejecting mode-3/nonzero-stride controls; renderer binding
admission test for the same ranges. [AMD RDNA2 ISA, vector buffer range
checking](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf).

The exact no-write condition is now proven separately: native numerical GPU
`--zero-stride-store-only` RED on the original emitter (sentinel overwritten)
and GREEN with the same oracle; CPU `--zero-stride-writer-alias-only` RED →
GREEN, preserving mode-3, nonzero-stride and atomic rejection; renderer
`buffer-stride-zero-overlap` RED → GREEN. Existing five zero-stride GPU cases,
four bounded zero-stride GPU candidates, full `resource_tracking_tests` and
`resource_materialization_tests` pass after correcting old test descriptors
that unintentionally used OOB mode 0 for live writer/output controls. Logs:
`_Build/logs/zero-stride-*20261002.log`. The renderer admission group test
could not pass its Windows child harness because some child processes failed
to reserve 13.8 GB before reaching the guard; direct targeted overlap and
atomic rejection children did reach and confirm their expected guard.

Native retry `_Build/runs/yotei-integrated-20261002-175224-presentprobe-gpuav`
on installed exe SHA-256 `4c92009ed90e9a6a41309c4fb45eff1ae6f6af98f85570d1d6a6832efd742919`
passes the prior mode-0/zero-stride alias candidate and creates the compute
pipeline for `34e090c623ad611c`, then exits 321 at shown 133 on a **live**
candidate: immutable source `0x5000f37f80+120`, writer the identical 120-byte
range, `STRIDE=8`, `NUM_RECORDS=15`, mode 0, origin 3, PC `0x1d94`.
This remains a cross-invocation alias problem; do not extend the OOB exception
to it. Readback starts at frame 180, so no current pixel/menu proof.
Independent source-image readback of frames 60–133 in
`_Build/runs/yotei-integrated-20261002-181225-presentprobe-gpuav` confirms
all are black, then reaches the same alias (dense resource 6 in that
specialization). There is still no nonzero frame or menu proof.

## Dynamic unsigned image sampler selection (2026-10-02)

After the clean scalar-buffer out-of-bounds and integer-sampler materialization
regressions passed, native run
`_Build/runs/yotei-integrated-20261002-160138-menucheck-gpuav-sync` still exits
321 at `vkCmdDispatchIndirect`: R8_UINT view has a linear sampler (VUID
`magFilter-04553`). Temporary binding diagnostics identify shader
`0x8968b4b53e5a246a`, image resource 48, Uint, paired to sampler 7 with
`force_point_filtering=1`. The SPIR-V emitter's
`CompatibleSamplerForImage` recognizes signed and converted images but omits
plain Uint, so its dynamic candidate can bypass that point variant.

Before changing emission, extend the synthetic indirect-image numeric-class
switch to assert the sampler descriptor index used by each sampled image:
float candidate uses the original sampler, Uint candidate uses its point
variant. Run the unchanged test RED on the current emitter, then GREEN after a
shared numeric-class correction; validate signed and float neighbors. Retry
the native game with source readback. The observed binding mismatch does not
prove a rendered frame or menu.

RED: `_Build/logs/indirect-uint-sampler-red-20261002.log`, exit 9, Uint
candidate bypassed the point sampler in emitted SPIR-V. GREEN: same
numeric-class switch with Float index 0 and Uint index 1 after the emitter
correction (`indirect-uint-sampler-green2-20261002.log`); mixed float/signed/
unsigned sampler materialization and neighboring indirect-image GPU test
also pass. A second native test build passes the same dynamic switch for
signed integer candidates (`indirect-integer-neighbors-20261002.log`). Installed
emulator SHA-256 `24c1f4ee5adc8813e375448674410f6d8943dae92508f2700de18c69445c1ebb`.
The real retry passes the previous R8_UINT Vulkan filter VUID and stops on
the independent immutable-SRT alias above before frame readback.

## Finite first-active-lane selectors through native EXEC guards (2026-09-28)

Committed 05d10203 retry `yotei-integrated-20260928-181147-menucheck-gpuav-sync`
passes d895 and reaches CS 86da5eb7b8257bb0, PC 0x530 BUFFER_STORE_DWORD.
Decoded selector uses V_READFIRSTLANE after finite per-lane choices and an EXECZ
early return. Existing finite-selector GPU fixture passes independently.
Hypothesis: nonempty-mask proof recognizes legacy low|high and Ballot equality,
but not the native ConditionRef EXEC reduction. Before production changes add
a CPU matrix for EXEC zero/nonzero, lower/upper masks and unrelated early exit;
keep missing/wrong/bypassed guards, mismatched masks, conditional read and cycles
rejected. Every snapshot root, bound and materialization limit remains required.
Do not admit arbitrary GPU-selected writes.

CPU RED confirmed on 05d10203 production: `native-exec-cpu-red-20260928.txt.stderr`,
exit 1, exact expected lost finite first-active-lane proof. Add native-CFG numerical
GPU full/upper-only fixtures with runtime unknown inactive VGPR values, matching
EXECZ guard and unchanged descriptor/output/sentinel oracles before production fix.
Both numerical native-CFG GPU fixtures RED before pipeline creation, PC 0x90
(`native-exec-gpu-red-FiniteSelectorNativeExecGuard{Full,Upper}-20260928.txt`),
exit 321. CPU legacy-profile audit of 86da independently reproduces the actual
PC 0x530 rejection (`srt-86da-cpu-audit-false-20260928.txt`); barrier=false remains
an explicit diagnostic assumption.

After guard admission, unchanged upper-only numerical fixture gives a second RED:
`native-exec-gpu-green-FiniteSelectorNativeExecGuardUpper-20260928.txt.stderr`,
272/7032 DWORD mismatches, assertion exit -1073740791. The full-mask variant passes;
the existing upper-only fixture without native guard remains GREEN. Split-wave
ConditionRef emission returns a per-lane predicate when other_half is null, even
though the admitted compute execution uses two physical native32 subgroups. Add
exact/one-byte-short mask-reduction scratch budgets to the existing planner matrix
before correcting emission and collective scheduling. Keep the numerical oracle.
Scratch-budget RED confirmed: `native-exec-ballot-budget-red-20260928.txt.stderr`,
assertion "mask ConditionRef omitted whole-wave ballot scratch" before production changes.

GREEN: unchanged CPU native-EXEC matrix (6 positives/12 negatives); full and upper-only
GPU fixtures pass all 7032 DWORDs with the original oracle. Existing finite selector
full/upper neighbors and bounded scalar store neighbor GREEN; cooperative LDS suite
9/9 and CTest 22/22 GREEN. Logs `native-exec-{cpu-green,gpu-wave-green-upper,final-*,
multiwave-green,ballot-budget-green,ctest}-20260928.*`. The 14-case planner matrix
now also enforces exact/one-byte-short mask ballot scratch budgets.
CPU translation of captured 86da passes both explicit diagnostic barrier profiles.
Fix shared native EXEC nonempty-path proof, split-compute whole-wave ConditionRef
reduction, collective rendezvous placement, requirements and scratch budgeting.
GPU-selected writes remain rejected unless the existing table/root/guard proof holds.
Native build `native-exec-wave-fix-build-20260928.log` GREEN. Committed 2e3a2a13
retry passes both descriptor-store failures and reaches the pending image boundary
at shown=101; captures of shown=0/66 remain black.

## Native image descriptor origin / split-latch post-test proof (2026-09-28)

Committed 2e3a2a13 native Windows retry reaches shown=101 and fails CS
5f3fdf61a7ca4a20, PC 0x26c: GetImageResource dword 0 rejects LoadAddressU32 root.
Run `yotei-integrated-20260928-183223-menucheck-gpuav-sync`, exit 321.
Before a production correction distinguish descriptor origin reconstruction,
finite/bounded table proof and guarded memory snapshots in a synthetic native-CFG
image regression; retain unknown/unbounded/conditional-root and writable-alias
rejections. Desktop captures remain black; no rendered image/menu claim.

Additional 2026-09-28 evidence: merge 10961866 real retry
`yotei-integrated-20260928-185252-menucheck-gpuav-sync`, maxShown=92,
black DesktopCopy shown=6/65, same image-origin exit 321. CPU-only captured-shader
audit also fails at PC 0x26c in both explicit legacy needs_lds_barriers profiles;
logs `srt-5f3f-cpu-audit-input-fixed-{false,true}-20260928`. Initial input_error
from a missing compute.needs_lds_barriers is excluded from reproduction evidence.
Next synthetic fixture must cover nested constant post-test loops with scalar image
address induction, canonical/native CFG origin, writes and invalid bounds. Keep
shader-write and runtime-value rejection until a supported proof is established.

Synthetic regression prepared before production correction: `resource_tracking_tests
--nested-posttest-image-only`. Four signed/unsigned/plain/non-nested positive cases
require dense [0,5) image table retention across an SSA split latch; six negative
cases retain nonunit step, wrong repeat edge, zero/runtime bound, entry bypass and
non-dominating update rejection. Code inspection shows the guest post-test update
and compare dominate an empty backedge block; current proof requires update.Parent
== latch even after proving update dominance. RED confirmed on unchanged 4a556c73 production: `nested-posttest-image-cpu-red-20260928.txt.stderr`,
exit 1, "split-latch nested post-test image loop lost its dense bound".

Native-CFG numerical regression prepared: `NestedPostTestImageDescriptorLoop`,
three full scalar-address image descriptors, signed outer/inner post-test loops,
two repeats per descriptor and independent six-output float oracle (2,20,80).
Wave32 fixture isolates descriptor proof; it does not claim new wave64 coverage.
All stores and table backing retained; loops have constant bounds. Before GREEN
run with only the new SrtWalker dominance correction absent to prove intended RED.

Corrected synthetic CFG still RED unchanged oracle on old proof:
`nested-posttest-image-cpu-corrected-red-20260928.txt.stderr`. Native numerical
fixture RED before pipeline creation at PC 0x38 GetImageResource root LoadAddressU32,
exit321 (`nested-posttest-image-gpu-red-20260928.txt`). Only the own SrtWalker
correction was absent; unrelated source preserved. The prior first GREEN attempt
stopped on a duplicate test edge assertion and is excluded from GREEN evidence.

GREEN on the same corrected fixtures and unchanged oracles: CPU 4/6 matrix;
native shader SPIR-V validation and numerical six outputs; captured 5f3f CPU audit
both legacy barrier profiles. Logs `nested-posttest-image-cpu-final-green`,
`nested-posttest-image-gpu-green`, `nested-posttest-capture-cpu-{false,true}`.
Shared correction replaces exact update-parent equality by proven latch dominance;
constant positive bound, unit-step phi, repeat edge, read dominance and runtime root
checks remain. Neighbor image table, wave64 image loop, bounded store3Sparse and
native EXEC upper GREEN; required CTest22/22 GREEN. Game retry pending.

Real committed c8aaaa5b retry passes 5f3f image-origin boundary, emits 150116 SPIR-V
words and creates pipeline successfully (2912ms); subsequent dispatches continue.
Run `yotei-integrated-20260928-202341-menucheck-gpuav-sync`, maxShown113,
black DesktopCopy shown37/106. No frame/menu/gameplay claim.

## Finite selector snapshot capacity corrected (2026-09-28)

Baseline c8aaaa5b committed retry failed MaterializeResources CS8457901d80b91921. stderr:
bounded SRT read1 exceeds snapshot limit, count65536 stride592 bias0,
accumulated words131072 vs word_limit65536. Guest code uses a finite 16-bit
lane-selected record key and multiple scalar-buffer descriptor/payload columns.
Hypotheses, not fixes: eager snapshot planning may cover a larger finite domain
than the descriptor extent, or ordinary payload dependencies may require a runtime
path instead. Before production change add synthetic finite selectors, descriptor
extents, OOB rows, required vs ordinary dependencies and cap endpoints. Preserve
read/store admission, zero-work semantics and snapshot/descriptor limits. Do not
claim a root cause or increase the limit without an independent regression.

### Regression required before capacity correction

`TestBoundedScalarProbeBudget` / `--bounded-scalar-probe-budget-only`:
retain 65536 logical keys in each of two scalar-buffer columns while an 8-byte
SRD permits only two coherent words; all other rows must remain zero. Repeat with
strides16/592, U32 offset wrap re-entering the extent, zero count and zero extent.
Keep exactly65536/plus-one in-bounds probe admission (including repeated addresses),
and exactly64MiB/plus-one dense storage admission. Failed materialization must
leave snapshot/specialization unchanged and never use ordinary mutable reads.
Run existing raw-address and workgroup probe limits unchanged.
Native GPU `FiniteScalarBufferDescriptorExtent` uses a GPU-loaded 16-bit key
0/1/65535, one-row scalar-buffer SRD and four descriptor columns; expected
output is the valid row then zero loads, with backing sentinels unchanged. Expected pre-fix
failure: second column rejected despite descriptor-proven zero rows. RED on unchanged production4db9964a: native CPU
`bounded-scalar-budget-red-20260928.txt.stderr` rejects read1 count65536,
stride16 bias4; native GPU `bounded-scalar-budget-gpu-corrected-red-20260928.txt.stderr`
rejects read1 at materialization before GPU execution. Both intended failures. An intervening GREEN attempt exposed a fixture-only
input address >=256, outside the harness packed offset field. Input relocated
without changing the key/data oracle; corrected fixture reran RED with only the
production patch absent. That harness failure is not GREEN evidence.
The proposed shared change charges descriptor-in-bounds probes independently of
dense storage, preserving logical rows and both existing constants. GREEN: unchanged CPU/domain oracle and corrected native GPU fixture
`bounded-scalar-budget-{final-green,gpu-final-green}-20260928.txt`; SPIR-V
validation and all backing words checked by the GPU harness. Native Windows
full build `bounded-scalar-budget-final-build-20260928.log` and required CTest22/22
`bounded-scalar-budget-ctest-20260928.log`. Scalar sparse store, upper EXEC guard,
and nested post-test image numerical neighbors GREEN. Existing raw/workgroup
exact probe boundaries pass unchanged. Original game retry remains pending.

Committed8cb79392 retry `yotei-integrated-20260928-205136-menucheck-gpuav-sync` confirms that
CS845790 passes81 bounded columns, SPIR-V54523 words and pipeline creation2041ms.
The domain/count remains65536; no quota constant changed. Source8cb79392
CI36482065792 Build/Test/Install GREEN Windows/Linux/macOS; macOS artifact upload
DNS failure prevents an overall GREEN claim. Documentation push retries CI.
New runtime boundary is below.

## Pending sampled pair image admission (2026-09-28)

2026-10-02 regression requirement before changing selector-bound lowering:
construct a synthetic inline descriptor table whose scalar selector is bounded
by a dominating unsigned CFG guard. Cover both `selector < limit` on the true
edge and `selector >= limit` on the false edge when the condition is wrapped in
`ConditionRef` (including `LogicalNot` polarity). Assert the exact finite
`selector_limit`, correct materialized image/sampler keys and pairs, and unchanged
behavior for a raw comparison. Opposite-edge, non-dominating, mismatched-selector
and EXEC/VCC-wrapped conditions must not yield an unsound finite bound. Exercise
multiple table roots whose candidate counts accumulate across the 512-image
budget, including equal/different samplers and exact/plus-one capacity. Obtain
native CPU RED on the current code before any production fix, then unchanged
GREEN; follow with a bounded native GPU selection check and game retry.
The source-level `ConditionRef` mismatch is a hypothesis until the RED/IR trace
establishes that it caused this runtime table's zero bound.
Native Windows CPU RED at source `98a46e46` with test-only fixture change:
`_Build/logs/inline-scc-guard-red-build-20261002.log` built
`resource_tracking_tests.exe`; `--inline-scc-selector-guard-only` exited 1 in
`_Build/logs/inline-scc-guard-red-20261002.log.stderr` with `buffer descriptor
is not a valid runtime value`. The control-flow guard is `selector < 3` on the
only edge reaching the table; this is the intended planner failure, not a build
failure. The relation to the game's exact current table remains to be checked.

Completed8cb79392 native Windows retry reaches PSf8927c09f4b928c7 at maxShown119,
then exit321: `inline sampled pairs exceed the dense image resource limit
(size=23184 stride=368 probes=1439 pairs=297 images=607)`. Evidence: completed
run above / stderr.txt and stdout tail. This failure is after the corrected CS
materialization and successful compute pipeline; no new frame/menu proof.

Read-only code diagnosis: BuildResourceSpecialization sums each indirect table's
candidate count minus its root before creating dense resources. Candidates carry
root-specific mapping/type metadata and an indirect sampler. It is not yet proven
whether607 represents necessary distinct typed pairs, duplicates across roots,
or an overestimated selector domain. Do not collapse images with different
samplers/types/roots blindly and do not increase MaxImages.

Before any future candidate deduplication or capacity change: capture candidate
identities/domain evidence with bounded CPU-only diagnostics; add independent
multi-root synthetic tables with overlapping
and disjoint image/sampler pairs, differing type/dimension/swizzle semantics,
invalid holes, descriptor extents and exact/plus-one admission. Prove intended
RED then unchanged GREEN. Native numerical image/sampler selection must preserve
per-root mappings and sampler differences, dirty-memory/alias rejection and
transactionality. Retry this PS in the game only after that proof.
Older diagnostic ISA capture:
`yotei-integrated-20260926-070451-presentfix-gpuav/shaders/original/0168_new_shader_ps_f8927c09f4b928c7.rdna2`;
current runtime memory values are not reconstructed from that ISA capture.

2026-10-02 GREEN: unchanged `--inline-scc-selector-guard-only` now passes after
the scalar `ConditionRef` lowering fix. New synthetic sampled image/sampler
table asserts both exact selector limits and three live pairs;
`--inline-sampled-scc-guard-only` passes. Polarity (`>=` false edge and nested
`LogicalNot`), mismatched selector, wrong edge, and EXEC/VCC rejection pass
`--inline-selector-guard-safety-only`; full `resource_tracking_tests` passes.
Logs: `_Build/logs/inline-scc-guard-green-20261002.log*`,
`inline-sampled-scc-guard-20261002.log*`,
`inline-selector-guard-safety-20261002.log*`, and
`inline-selector-full-resource-tracking-20261002.log*`. The sampled test was
added after the minimal shared buffer RED; it has no separate recorded RED.
Multi-root exact/plus-one image admission and non-dominating guard remain
synthetic coverage debt. In the native game retry
`yotei-integrated-20261001-214503-menucheck-gpuav-sync`, this PS did pass
resource specialization (386 images, 383 pairs) and emitted 295838 SPIR-V
words; a later VS SRT failure stopped the game before readback.

## Planning-only scalar-address SRT slot (2026-10-02; regression resolved)

The completed retry above reaches VS `ee4f153aa500d327` at maxShown170 and
fails in `RefreshFlatBuffer`: flat slot2 is a raw planning-only
`LoadAddressU32` (`opcode=209`, `kind=ScalarAddress`, `slot_kind=0`) whose eager
evaluation fails. Evidence: that run's `stderr.txt:1` and `_kyty.txt:8182384`.
This is distinct from the now-passed sampled-image admission. An older generic
null-root correction allowed this shader to emit SPIR-V, but the current
descriptor value and memory state have not been reconstructed. Do not turn
arbitrary unreadable planning-only memory into zero.

Before the production change, an independent CPU regression was added for a
planning-only raw scalar-address read from an optional exact-null root that is
unneeded by the active resource path, plus a used/readable root control.
Non-null unreadable, dirty, aliased or actively required roots must still fail
closed without partially committing a resource snapshot. Distinguish a slot
that is truly unused/deferred from a required descriptor word. The native
`--conditional-planning-srt-only` test gave RED on the old implementation and
GREEN unchanged after the CFG owner/reachability fix. Active/unreadable,
unknown-condition, shared-owner, shader-write and repeated-snapshot controls
are GREEN; full resource tracking/materialization suites, native build/install
and three neighboring GPU compute cases are GREEN. In the bounded game retry
VS `ee4f153aa500d327` emitted SPIR-V, while readback and a visible frame
remain unproved. Logs and run references are in the first launch-plan checkpoint.

## Pending clean scalar-buffer flat slot (2026-10-02)

The next bounded retry stops at CS `7ceb0f3417f926f9`, flat slot31:
`ReadConstBuffer`, `FlatSlotClean`, planning-only scalar buffer read, PC `0x294`,
memory index60. `RefreshFlatBuffer` returned failure before SPIR-V for that
specialization. The current error does not distinguish an invalid descriptor
extent, unreadable guest address, unavailable argument or another evaluator
failure. Older saved variants of this shader compiled, but they are not proof
that the current runtime value is valid.

Before changing production behavior, capture the evaluated descriptor base,
stride, record count, offset and exact failure reason without exposing guest
data. Add synthetic RED controls for a required readable clean slot and the
actual failing case once its semantics are known; keep descriptor bounds,
unreadable memory and failed argument evaluation closed and transactional.
Do not apply ordinary-slot reachability skipping to clean slots: bounded
wave-uniform resource planning may consume them outside the shader block.

Diagnostic retry `yotei-integrated-20261002-152819-menucheck-gpuav-sync`
(`f3f71319` plus diagnostic-only SrtWalker diff, installed exe SHA-256
`421ac459e84c220c8512ecab1884578b57ddb014175429d2b2204b09a7331779`)
reaches maxShown177 and gives the exact failure: PC `0x294` clean scalar-buffer
read, descriptor `base=0x5000000000`, `stride=8`, `records=0`, byte offset 0,
computed extent 0. No guest-memory callback was reached. The AMD RDNA2 ISA
scalar-buffer contract uses base, stride and num_records for bounds, and the
existing GPU `EmitReadConstBuffer` path returns zero for an OOB DWORD.

Before changing the CPU evaluator, add a minimal clean flat-slot regression
with a nonzero base and zero records. It must return the guest's OOB zero with
no memory probe, while a valid one-DWORD descriptor returns the real memory
word. Cover an offset at the exact end, one partially covered DWORD, stride
zero and nonzero, and an in-bounds unreadable/dirty specialization read that
still fails transactionally. Run unchanged RED with the current evaluator,
then unchanged GREEN after a shared bounds correction; do not treat invalid
descriptor shape, negative offset or arbitrary unreadable in-bounds memory as
an OOB zero. Repeat the native game with readback and pixel evidence.

Completed CPU proof: `resource_tracking_tests --clean-scalar-buffer-oob-only`
gave intended RED before the evaluator change (`clean-scalar-oob-red-20261002.log`,
exit 1), then unchanged GREEN. Cases cover zero records with a nonzero base
and both accepting/rejecting memory callbacks, exact-end and partial-DWORD
OOB, valid stride-zero/nonzero reads, and transactional in-bounds unreadable
failure. Full resource tracking/materialization suites GREEN. Native retry
`yotei-integrated-20261002-153834-menucheck-gpuav-sync` on installed SHA-256
`555231ceff7f78cd06daf678c18d305ed1385181ff371ca279ed8421d7e48e78`
emits SPIR-V for `7ceb0f3417f926f9` (`_kyty.txt:6759215`) before the
separate sampler validation error. This verifies passage of the old SRT
boundary in that run; it does not prove a visible frame.

## Pending unsigned-integer sampled-image filtering (2026-10-02)

The OOB-corrected retry `yotei-integrated-20261002-153834-menucheck-gpuav-sync`
passes the old clean SRT read (`7ceb0f3417f926f9` emits SPIR-V), then Vulkan
validation aborts at maxShown131: a `VK_FORMAT_R8_UINT` sampled view is paired
with a linear-filter VkSampler in `vkCmdDispatchIndirect` (VUID 04553).
This is an independent shader specialization/binding issue; no source readback,
visible frame or menu was proved. `BuildSamplerPlan` maps unconverted `Uint`
images to `SamplerClass::Integer`, which sets integer border color but leaves
linear filtering. Signed or converted integer images use `PointInteger`.

Before production change, run the existing `ImageSampleR8UintForcesPointSampler`
numerical fixture through a focused native CLI and prove its sampler-specialization
expectation fails on current code. Preserve its output oracle. Cover a float
sample with the same guest sampler and the packed-integer conversion case so
point filtering is selected only for integer image bindings. Check Vulkan
format-feature constraints rather than assuming every integer view supports
linear filtering. Repeat the exact game profile after unchanged GREEN.

Focused native CLI `--integer-sampler-point-only` gave intended RED before
production change: `ImageSampleR8UintForcesPointSampler` failed at sampler
specialization (`integer-sampler-red-20261002.log`). Shared correction forces
point filtering for unsigned and signed integer sampler variants while
preserving the float variant. The same R8 UINT numerical oracle, packed UINT
sample/gather, float sample/gather and mixed float/UINT/SINT one-source
binding case pass (`integer-sampler-mixed-green-20261002.log`). The mixed
case's old "UINT may remain linear" expectation was corrected under Vulkan
VUID 04553. Full resource tracking/materialization suites and native emulator
build/install also pass. The retry `yotei-integrated-20261002-155304-menucheck-gpuav-sync`
on SHA-256 `c1950a59fa01337d595dea72246cf5256f533c63f41bf8027e2eb05fbe7e7dda`
still hits VUID 04553 for `image_10[3]` / `R8_UINT`, maxShown116. Thus the
focused UINT sampler correction did not cover this runtime binding; retain it
as test-proven but runtime-unvalidated. Before further production change,
diagnose the actual binding-10 image resource class, view format and all
sampled-pair sampler variants for the failing dispatch. The shader has 64
images and 50 sampled pairs in that specialization; do not infer the bound
pair from the test's single-image fixture. Make a synthetic RED from the
identified shared mapping/format mechanism, then retry.

## Pending prepared-frame diagnostic readback layout (2026-10-02)

With readback starting at frame1, the game stops at Vulkan validation:
`copyImageToBuffer` expects `TRANSFER_SRC_OPTIMAL`, but the prepared image is
still in `TRANSFER_DST_OPTIMAL`. The no-readback retry passes that point. This
is a diagnostic-presentation blocker, separate from guest SRT and the goal of
proving actual pixels.

Before changing the capture path, add a bounded synthetic presentation
regression that prepares a frame by copy and by clear, captures it with Vulkan
validation, checks the copied bytes and the frame's later presentation, and
reuses the frame. Include guest-source capture and ensure image layout/access
transitions are recorded on the command buffer that performs the readback.
The current game run is a reproduction but is not the minimal synthetic test.

## Pending neighboring unaligned scalar-buffer read (2026-09-28)

Additional numerical test `--unaligned-scalar-buffer-load-only` fails 1024/1032
DWORDs: expected 0x13579bdf at host byte adjustment 3, actual 0xdf579bdf from offset 0.
`native-exec-unaligned-scalar-neighbor-20260928.txt.stderr` reproduces it; the
same unchanged test still fails identically with only the own wave-emitter,
cooperative scheduling, requirements and budget patch absent
(`native-exec-unaligned-baseline-red-20260928.txt.stderr`). Source read-only
comparison to 2fc423a9 shows that EmitReadConstBuffer already omitted host-view
byte adjustment before the fresh merge. This neighboring path is not GREEN.
Before a separate fix retain that RED and add host adjustments 1/2/3 plus
aligned and end-of-view cases; use shared bounds-checked byte reconstruction,
preserve guest SMEM offset semantics and avoid shifting the output resource.
The wave patch was preserved and restored via
`_Build/analysis/native-exec-wave-production-20260928.patch`; no reset/user edits.

## Bounded descriptor stores through scalar ConditionRef (2026-09-28)

Retry on 5b63741c (`yotei-integrated-20260928-174752-menucheck-gpuav-sync`)
rejects CS d8959888aafd2552 at PC 0x1b4, BUFFER_STORE_DWORD. This is a store,
not the single-DWORD read assumed during initial diagnosis. Preserve the GPU-selected
write rejection. CPU-only legacy-profile diagnostic reproduces the same boundary;
its needs_lds_barriers=false is an explicit diagnostic assumption, not a new capture.
The bounded loop proof unwraps LogicalNot but not scalar ConditionRef introduced
by upstream CFG. Before production edits, add six positive descriptor-store loop
cases (SCC zero/nonzero, unsigned/reversed/signed comparison) and eight rejection
neighbors (wrong success edge, pre-guard read, nonunit step, guarded pointer).
Keep live induction keys, host snapshot limits and runtime store semantics.
CPU RED confirmed: `bounded-condition-cpu-red-20260928.txt.stderr`, exit 1;
first SCC-wrapped descriptor store loses its bounded table and fails host validation.
The unrelated x1-read experiment is preserved outside source in
`_Build/analysis/indirect-dword-experiment-20260928.patch` and reverted from source.

GREEN: unchanged six positives/eight rejection neighbors; four additional EXEC/VCC
reduction cases remain rejected (`bounded-condition-cpu-boundaries-green-20260928.txt`).
Unwrap only SCC ConditionRef and preserve polarity from its already-decoded operand.
Numerical native-CFG BoundedBufferScalarLoopStore{1,3}{Full,Sparse} pass complete
output/SRT/sentinel comparison and SPIR-V validation. Four existing zero-stride
Raw/Formatted/D16/Scalar neighbors GREEN. Captured d895 CPU resource tracking
passes both explicit barrier diagnostic variants; materialization/GPU/game proof
is not inferred from that CPU audit. Native build3 and CTest 22/22 GREEN
(`bounded-condition-fix-build3-20260928.log`, `bounded-condition-ctest-20260928.log`).


## GPU-selected raw buffer fallback integration (2026-09-28)

Game retry on a233dfe1 `yotei-integrated-20260928-173637-menucheck-gpuav-sync`
passes the ordinary-payload SRT failure and reaches CS d8959888aafd2552,
pc 0x1b4: dynamic LoadAddressU32 buffer dword fails host validation.
Upstream already provides a GPU descriptor path limited by
SupportsIndirectBufferLoad to raw DWORD x2/x3/x4, but local GetHandle always
throws instead of returning its new false result to that guarded caller.
Before changing production run existing numerical BufferLoadsGpuSelectedDescriptors
and BufferLoadDwordx3GpuSelectedDescriptors; preserve x1/formatted/typed/store
rejections and bounded-table proof failures. Do not return a null host resource.

Existing numerical fixture RED: `indirect-buffer-red-20260928.txt`, exit 321
at PC 0x30, dynamic ReadConstBuffer descriptor rejected before pipeline creation.
Add CPU admission matrix for raw x2/x3/x4 versus x1/formatted/typed/store,
asserting supported loads use IndirectBuffer without a fake host binding.

CPU matrix RED confirmed: `indirect-buffer-cpu-red-20260928.txt`,
raw x2 fails GetBufferResource runtime validation. Return false only for a
buffer source validation failure after existing bounded proofs; the caller
still enforces the raw vector contract before activating the GPU path.

After admission, the unchanged GPU fixture reaches a second integration RED:
`indirect-buffer-gpu-green-20260928.txt` asserts in host buffer remapping at
ResourceMaterialization.cpp:2788. GPU-selected handles have no host dense index.
Extend the CPU matrix through materialization/specialization before correcting
that remap; raw GPU handles must retain their descriptor operands and kind.

CPU plan extraction RED: `indirect-buffer-specialization-red-20260928.txt`
throws invalid vector subscript in ResourceControlFlow: it also assumes an
IndirectBuffer has a host source index. Exclude only IndirectBuffer from host
source liveness and host remapping; preserve dense/table validation for other
kinds. The unchanged GPU fixture independently reproduces the later remap.

GPU-selected buffer GREEN: unchanged CPU admission/materialization matrix
and full resource suite (`indirect-buffer-specialization-green-20260928.txt`);
numerical x2/x4 and x3 fixtures pass (`indirect-buffer-final-*.txt`), including
11 descriptor variations, bounds modes, swizzle, OOB tails and unmapped rows.
Mandatory CTest 22/22 (`indirect-buffer-ctest-20260928.log`).

## Ordinary scalar payload prefetch (2026-09-28)

Retry on 76d56359 reaches CS 8968b4b53e5a246a, shown=0, flat slot 209.
Temporary bounded diagnostics (`yotei-integrated-20260928-172639-menucheck-gpuav-sync`)
show a host read of address zero at guest PC 0x39dc. The native instruction
is scalar payload under an EXECZ branch, not a descriptor dependency. The
new planner blanket-hoists constant-offset scalar loads before guest control
flow, even when only arithmetic consumes them. Add an unchanged CPU matrix
proving ordinary payload loads stay in guest IR while descriptor dependencies
still receive valid flat slots; numerical scalar-read/cooperative neighbors and
game retry must confirm the shared correction. Do not fabricate zero memory.

CPU RED confirmed: `scalar-payload-red-20260928.txt` rejects eager hoisting
of an ordinary scalar payload. Descriptor dependency is the positive neighbor
in the same unchanged fixture. Restrict host prefetch to resource recipes,
retaining ordinary scalar memory and its guest control-flow position.

Neighbor input correction: numerical BDA coefficient test RED
`scalar-payload-gpu-cooperative-bda-coefficients-20260928.txt` (2304/16516
words differ; coefficient planes pass, mailbox reads return mode-0 defaults).
The fixture replaces the default user-data array but omits word 51: its
zero-stride vector descriptor therefore explicitly requests bounds mode 0,
which correctly always returns OOB zeros. Set its raw byte-address descriptor
to bounds mode 3, matching MakeNativeUserData/MakeStructuredStorageBufferData;
retain all 16516 numerical expectations, alias separation, dispatch geometry
and real mailbox feedback. This repairs the fixture input, not production OOB
semantics. Mode-0 bounded Raw/Formatted/D16/Scalar remain GREEN unchanged.

Scalar payload GREEN: unchanged CPU matrix (`scalar-payload-green-20260928.txt`),
required CTest 22/22 (`scalar-payload-ctest-20260928.log`), bounded numerical
mode-0 Raw/Formatted/D16/Scalar 4/4 (`scalar-payload-bounded-*-final-20260928.txt`),
cyclic scalar address GPU and cooperative LDS 9/9
(`scalar-payload-multiwave-green-20260928.txt`). Corrected mode-3 BDA neighbor
passes all 16516 unchanged values (`scalar-payload-bda-fixture-green-20260928.txt`).
Temporary SRT diagnostics removed before the fix; original logs preserved.

## Wave64 ConditionRef admission (2026-09-28)

Game retry on 4fcc3575 reaches a controlled ConditionRef rejection, shown=0:
`yotei-integrated-20260928-171407-menucheck-gpuav-sync` (CS a7661ff4ea282325).
Before changing production add a synthetic CPU matrix of native mask conditions
with varying lane predicates and ScalarInstruction with uniform/varying inputs.
Mask conditions already reduce both halves in the backend; scalar instruction
conditions must retain their operand uniformity and divergent rejection.
CPU RED confirmed: `condition-ref-red-20260928.txt`, intended assertion
"native ConditionRef lost whole-wave branch reduction in cooperative planning".
Run the fixture RED/GREEN unchanged, existing scalar-mask numerical/Spir-V
cases, cooperative admission, and repeat the original game.

Neighbor fixture API updates: `condition-ref-cooperative-20260928.txt`
asserts while constructing retired four-argument DataAppend (now two args);
`condition-ref-mask-oracle-old-20260928.txt` asserts on retired integer mask
IR. Adapt fixtures to current signatures and ConditionRef kind/predicate,
retaining input matrix and rejection oracles. These are fixture compatibility
failures, not additional production RED evidence.

ConditionRef GREEN: unchanged 14-input admission matrix and six neighboring
CPU/CFG selectors (`condition-ref-neighbor-*.txt`), mandatory CTest 22/22
(`condition-ref-ctest-20260928.log`). Numerical GPU: wave64 subvector loops
and VCC branch pass; multi-wave LDS/cooperative selector 9/9 passes
(`condition-ref-multiwave-gpu-20260928.txt`). No unsupported-case guards removed.

## Fresh upstream 22ff integration regressions (2026-09-28)

Native merge build succeeds (`merge-22ff-build3-20260928.log`). Existing unchanged
resource suite is RED: `invariant indirect images`, address-backed image words at
pc 0x1220 fail runtime-source admission (`merge-22ff-ctest1-20260928.log`, 21/22).
Retain the finite scalar-buffer key enumeration when the image table has a
scalar-address root and positive table offset; the new lane/loop proof remains
required for domains without that independent finite material-buffer bound.
Do not weaken negative immediate, alignment, readability, probe or alias checks.
The preserved address-table fixture includes a shifted key followed by a constant
image-table bias; exclusive-use proofs must cover that complete offset chain
(`merge-22ff-resource2-20260928.txt`, same intended admission RED).

The bounded Raw selector fails before its own fixture is compiled: construction
of an unrelated upstream FP64 case eagerly compiles a vertex reciprocal chain
with unknown initial guest FP state. Log `merge-22ff-zero-raw1-20260928.txt`,
exit 321. Preserve this strict FP64 rejection; move the companion compile check
into the selected case execution so fixture enumeration has no compiler side
effects. Numerical bounded Raw/Formatted/D16/Scalar oracles remain unchanged.

The old shared-exit test's route-field counters describe the retired CFG
representation. Adapt it to the new expression/assignment API by comparing
executed native instruction paths for every branch-decision combination,
retaining instruction coverage and validated structured SPIR-V checks.

Numerical GPU RED after companion isolation: `merge-22ff-zero-raw2-20260928.txt`
returns the first candidate's payload for both workgroups. The dump
`_Build/analysis/merge-22ff-zero-raw-red.spv{,.ir.txt}` shows the bounded
GetBufferResource selector scrubbed to zero by upstream's descriptor cleanup.
Preserve runtime keys for bounded buffer/image/sampler and inline tables;
ordinary host-only descriptor words can still be discarded. Repeat the same
four numerical oracles unchanged and neighboring descriptor-selection cases.

Address-table oracle correction: RDNA2 ISA section 7.2.1 describes the memory
address as base + immediate + scalar offset, without truncating the resulting
sum to U32. The shader's index multiplication wraps before SMEM; adding its
positive immediate must not invent a second wrap. The old address fixture's
112-byte key is unreachable (minimum offset 368). Keep that word as a rejected
read trap and assert only OOB key zero and the real 368-byte key five are mapped.
This follows the same widened addition already covered by TestConstantBufferBounds
and upstream's split-offset matrix. Source:
https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf.

Legacy preplanned-IR regression RED: `merge-22ff-resource4-20260928.txt`
accepts a formatted vector record key because unconditional tracking DCE removes
the unused image query before its strict resource validation. Preserve the
planning-only API contract: run this new DCE only when tracking itself builds
the SRT plan, and rerun the unchanged resource rejection/alias fixtures.

Record-key alias regression RED: `merge-22ff-resource5-20260928.txt`
accepts a written buffer alias of an indirect record key. Include record-key
indirect images in the specialization capture policy, alongside selector masks;
retain the unchanged rejection fixture and non-alias positive cases.

Integer sampler integration RED: `merge-22ff-verified-neighbor-sampler-border-20260928.txt`
exits 321: UInt border-color image has no point variant. The new materializer
classifies UInt as Integer (integer border, original filtering), Sint/converted
as PointInteger. Match backend point routing to that existing classification;
rerun unchanged border-color numerical cases and indirect-image neighbors.

Integration GREEN: unchanged resource suite passes (`merge-22ff-resource6-20260928.txt`),
required CTest 22/22 (`merge-22ff-ctest2-20260928.log`), neighboring upstream
CPU suites 12/12 (`merge-22ff-extra-ctest-20260928.log`), shared CFG execution
and SPIR-V checks pass (`merge-22ff-shared-cfg-20260928.txt`). Four numerical
bounded-buffer cases and five neighbors pass (`merge-22ff-verified-*.txt`);
integer border fix passes `merge-22ff-sampler-border-green-20260928.txt`.
Full compute suite remains unverified: known strict FP64 and comparison-depth
boundaries are preserved, not bypassed by this integration.

## Candidate-specific zero-stride vector reads (2026-09-28)

Before changing production, add numerical native GPU cases
`BoundedBufferZeroStride{Raw,Formatted,D16}`: two dispatch workgroups
load distinct descriptors from a separate immutable SRT, one normal and one
mode-0 zero-stride. Preserve ordinary data, per-candidate float SEL_1 defaults,
D16 packed defaults, inactive EXEC sentinels and all untouched SRT/backing words.
Require table selection in validated SPIR-V. RDNA2 ISA section 8.1.5, Table 35
and range-check notes specify offset >= stride for mode 0 and SEL_1 exceptions:
https://docs.amd.com/api/khub/documents/Et~wpu9g~Ffl7d9q0QZ~Og/content.
Run unchanged oracles RED before lowering and GREEN afterwards; retain direct
zero-stride/scalar and ordinary table boundary checks. GPU work must be bounded.

RED on unfixed production d690678c: all three selectors exit 321 at the
explicit bounded zero-stride rejection, without dispatch or driver failure.
Logs: `_Build/logs/bounded-zero-{raw,format,d16}-red-20260928.txt` and
`.run.json`. The temporary CPU unsupported-case oracle can now be replaced
by retention of per-candidate OOB metadata, covered by these independent
GPU numerical fixtures. GREEN pending.

After admitting candidate-specific OOB, the unchanged Raw fixture exposes
a second compiler RED: `GetBufferResource has an empty argument`
(`bounded-zero-raw-green-20260928.txt`, exit 321). Workgroup-bounded
columns have a dispatch-axis bound and no scalar count root; the typed handle
must retain valid planning operands without inventing a descriptor count.
Preserve the numerical fixture and distinguish its dispatch bound from loop
count roots before correcting handle construction.

The first formatted emit attempt fails SPIR-V validation: vector OpSelect
requires a matching condition shape. Use the existing conditional value
merge, retaining the same sparse-EXEC oracle. Also correct the synthetic D16
encoding to set opcode bit 25 (the helper masks high bits), add a decoded
opcode assertion, and reproduce the corrected D16 case with the candidate
production patch absent. Add an independent bounded scalar read: mode-0
vector OOB must not erase scalar-buffer data.

Corrected D16 RED with all candidate production changes absent:
`bounded-zero-corrected-d16-red-20260928.txt`, exit 321 at the original
mode-0 rejection. Preserved patch: `_Build/analysis/bounded-zero-production-in-progress-20260928.patch`.
Bounded scalar numerical RED: `bounded-zero-scalar-red-20260928.txt.stderr`
returns zero for all four lanes of the second workgroup instead of 0x40000000.
Keep ScalarBuffer accesses on their existing scalar bounds path.

Native numerical GPU GREEN: all four cases, unchanged final oracles,
`bounded-zero-{raw,formatted,d16,scalar}-final-20260928.txt`, exit 0.
The corrected D16 fixture retains its decoded-opcode assertion. All emitted
modules pass Vulkan 1.2 SPIR-V validation. Neighbor selectors GREEN:
zero-stride-oob, buffer-format-store, buffer-d16, indirect-image; logs
`bounded-zero-neighbor-*-20260928.txt`. Executable SHA-256:
`536463B7358654AD1CC7E5088C352FE17D248A56AA4804BEF7789D8A31BE5619`.
Native emulator/launcher/kyty_tests build and install GREEN:
`bounded-zero-native-{build,install}-20260928.log`. Focused CTest GREEN
22/22: `bounded-zero-native-ctest-20260928.log`, including required Windows
checks and existing resource tracking/materialization boundaries. Original
game retry remains pending at this checkpoint.

Separately, exact-head CI d690678c failed Linux compilation at two test-only
zero-argument optional emplace calls (Windows/macOS succeeded). Use explicit
aggregate initialization without changing semantic expectations. Evidence:
`_Build/logs/ci-d690-linux-failure.txt`.

## Upstream integration and SRT rejection regressions (2026-09-27)

Native required checks on the merge of `e0c73250` and `421684e7` expose
unchanged CPU RED cases: `TestConstantBufferBounds` accepts an out-of-bounds
planning read, and the wrapped scalar-buffer immediate in
`TestInvariantIndirectImages` passes runtime admission. Evidence:
`_Build/logs/merge-focused-ctest-20260927.log`. Before correcting production,
preserve these rejection oracles, retain in-range reads, and reject unreadable
non-null planning roots rather than fabricating zero descriptors. Scalar-buffer
admission must agree with the evaluator's unsigned offset constraint. The
RO-upload fixture must contain a live memory access under upstream binding
pruning; its byte/padding oracle remains unchanged. GREEN and game retry pending.

After restoring those rejections, the existing `TestUniformScalarBufferImage`
also exposes RED: draw-uniform nested descriptors are captured by inline GPU
descriptor lowering instead of normal runtime materialization. Preserve its
three numerical descriptor-address oracles and the unreadable-memory rejection;
uniform descriptors must keep the ordinary specialization path.

The next existing RED is `TestSrtRawFallbackReadability`: the no-callback
SRT fallback directly dereferences null/reserved/no-access host memory, causing
native CPU AV. Evidence: `merge-resource-crash-localize-20260927.txt` and the
existing `--srt-raw-fallback-case` fixtures. Require an exact, fallible DWORD
copy through the host OS, preserve successful readable bytes and transactional
failure, and retain callback-based guest-memory reads. This is CPU memory
admission, not a GPU-driver workaround. GREEN pending.

Existing finite-selector rejection RED (`merge-resource-finite-red-20260927.txt`)
shows that a four-column descriptor with a displaced fourth column bypasses
the failed correlated-table proof via generic bounded-expression admission.
Preserve mixed-column/mixed-index, unknown/undef/cyclic/conditional-root and
stage rejection fixtures; retain positive contiguous columns and genuine
derived bounded descriptor expressions. No broader descriptor assembly is
claimed without an independent regression.

Existing finite-selector materialization RED then exposes an immutable-source
writer alias accepted by the `bounded_srt_reads_precede_writes` bypass. A local
CFG ordering observation is not a dispatch-wide ordering proof across waves
or workgroups. Preserve exact-end nonoverlap, readable candidate snapshots,
transactional failure and the last-DWORD overlap rejection before changing
alias admission. Evidence: `merge-resource-correlated-green-20260927.txt.stderr`.

Next RED: `TestWorkgroupSrtTrackingProof` rejects an otherwise proved acyclic
WorkgroupId read solely because its CFG has no cycle. Preserve the same affine
proof and coherent snapshot/alias checks for acyclic and cyclic inputs;
host-read failures must remain errors. Evidence: `merge-resource-alias-green-20260927.txt.stderr`.

Existing transactional-limit RED: `TestBoundedMaterializationLimitsAreTransactional`
accepts 65,538 logical probes across two columns after the WIP raised the
combined budget. Preserve the unchanged 65,536 success and 65,538 rejection
oracles, including repeated-address memoization and unchanged snapshots on
failure. Keep allocation capacity separate from the logical probe budget.
Evidence: `merge-resource-workgroup-green-20260927.txt.stderr`.

Conditional-buffer RED (`merge-resource-budget-green-20260927.txt.stderr`)
shows that the WIP lost upstream reachability-aware descriptor materialization.
Preserve existing untaken/taken, absent descriptor, failed predicate read, shared
use, loop and writable-alias cases. Only proved unreachable sources may skip
evaluation; unknown predicates keep every potentially executed source.

Existing image-binding ABI RED (`merge-resource-indirect-green-20260927.txt.stderr`)
accepts a depth-comparison storage image. Preserve the existing comparison,
integer, atomic and dimension rejection matrix; comparison sampling applies
only to sampled resources.

Native `resource_tracking_tests` GREEN: all unchanged rejection and positive
fixtures pass after the shared corrections, including eight raw host-memory
readability cases. Evidence: `_Build/logs/merge-resource-image-abi-green-20260927.txt`
and its `.run.json` (exit 0). The added unreadable planning-root regression
was recorded RED before its fix. Full rebuilt focused checks and GPU cases
remain pending at this checkpoint; game progress is not inferred from CPU GREEN.

SDWA MOV numerical fixture RED: the decoder rejects byte source extraction
combined with partial destination insertion, although shared translation
already treats both fields independently. Preserve the unchanged overlapping
source/destination, sign extension and inactive EXEC oracles before admitting
those combinations. Evidence: `merge-gpu-sdwa-mov-20260927.txt`.

FP64 fixtures need descriptor setup aligned with upstream zero-stride mode-0
semantics: their explicit user data overwrites default mode-3 bounds bits.
Retain every IEEE literal oracle and sentinel, set raw-buffer bounds mode 3
explicitly, and retain the separate zero-stride mode-0 rejection GPU fixtures.
Evidence: `merge-gpu-f64-arithmetic-20260927.txt.stderr` and conversion log.

Indirect-image selector RED is a host test AV (`merge-gpu-indirect-image-20260927.txt`,
Application Error fault module is the test EXE). A diagnostic native link map localizes the AV to `EmitImage`: ordinary
non-comparison sampling unconditionally reads an empty optional specialization
array for `needs_manual_depth_compare`. Preserve the unchanged indirect-image
fixtures; only comparison instructions with supplied specialization need that
lookup. Evidence: `merge-indirect-map-crash-20260927.txt` and
`_Build/analysis/indirect-diagnostic.map` (fault RVA 0x3a633e).
Separately, the fixture's SPIR-V assertion treats an `OpConstant` literal as an
SSA ID while inspecting LOD. Accept direct float constants and equivalent
bitcasts with bounds checks, preserving exact 2.0 LOD and coordinate dimensions.

GPU checkpoint: SDWA MOV GREEN (`merge-sdwa-green-20260927.txt`), all ten
FP64 conversion cases GREEN (`merge-f64-conversion-green-20260927.txt`).
FP64 arithmetic passes multiply, fused FMA, signed-zero FMA and F64-to-F32
rounding, then `RcpF64OddConvertedIntegersWithinIsaError` terminates with
`0x80000003` inside NVIDIA `nvgpucomp64.dll` 32.0.16.1714 at offset 0x589eb2.
Evidence: `merge-f64-arithmetic-green-20260927.txt` and its `.run.json`;
Windows Application Error event confirms the driver module. Do not claim the
arithmetic selector GREEN; an isolated saved-SPIR-V probe remains required.

After removing that AV, the unchanged mixed-candidate key-switch test is RED:
`merge-indirect-emitter-green-20260927.txt.stderr` reports missing shared cube
coordinate conversion. The candidate sample path calls `CoordF32` without
the selected image cube metadata. Preserve both cube-first permutations,
the two coordinate-subtraction oracle, exact LOD and three sample cases;
apply guest cube conversion for each candidate from decoded resource metadata.

The same unchanged LOD oracle then exposes RED at mixed coordinate/LOD layout
(`merge-cube-candidate-green-20260927.txt.stderr`): a 2D candidate relocates
the LOD slot of an instruction decoded as 2D-array. Guest address operands
follow decoded instruction metadata; candidate dimensions change host coordinate
width, not the instruction LOD/bias/gradient slots. Preserve LOD=2.0 for
both candidate orders before correcting the shared sample operand layout.

Indirect-image selector GREEN after the shared emitter corrections:
`merge-sample-layout-green-20260927.txt`, exit 0. Both mixed cube orders,
exact LOD assertions and the numerical cube-gradient GPU case pass. Temporary
disassembly instrumentation was removed; its RED artifact remains in `_Build/logs`.

Neighbor check `--sampled-depth-resource-only` remains RED at the existing
`ComparisonDepthTexture` assertion requiring `OpImageSampleDrefExplicitLod`:
`merge-sample-depth-neighbor-20260927.txt.stderr`. Manual depth-compare lowering
is already present in pre-merge `e0c73250`; distinguish host format capability
admission and the fixture opcode requirement from numerical comparison results
before changing either. Its GPU numerical stage has not been reached.

Game retry on `1f1f7665` reaches a binding-collector bounds error before present:
`_Build/runs/yotei-integrated-20260927-201152-menucheck-gpuav-sync`, exit 321.
A specialized bounded buffer access has a logical table and multiple dense
candidates, not one `memory.resource`. Before changing collection, add a CPU
fixture with reordered/duplicate candidates, a separate direct buffer, and
invalid table/candidate boundaries. Require only live dense resources in
bindings, preserving their ascending compact order and packed shader-data ABI.

Bounded-binding regression RED/GREEN: unchanged valid fixture fails on the
pre-fix collector (`merge-bounded-bindings-valid-red-20260927.txt`) and passes
with candidate collection (`merge-bounded-bindings-valid-green-20260927.txt`).
Full native rebuild, shader validation and original game retry remain required.

Second game retry (`yotei-integrated-20260927-201838-menucheck-gpuav-sync`,
`e47a15c7`, exit 321) passes the collector and exposes a CPU AV in
`ApplyResourceSpecialization` (RVA 0x268ce7): upstream zero-stride read rewriting
indexes a logical-table access as a direct dense resource. Before fixing it,
add empty/nonempty vector-table application tests with preserved live reads.
A vector table containing a mode-0 zero-stride candidate needs candidate-specific
zero/default-format lowering; reject that unsupported combination explicitly
until its independent numerical GPU regression exists. Do not disable the
existing direct zero-stride rewrite or treat every table candidate as zero.

Vector-table application CPU RED reproduces the native AV without Vulkan:
`merge-table-vector-red-20260927.txt`, exit 0xc0000005. Unchanged GREEN:
`merge-table-vector-green-20260927.txt`, exit 0, empty and nonempty table reads
remain explicit and mode-0 zero-stride candidates fail with the supported-case
boundary. Candidate-specific zero/default-format GPU lowering is pending.

Final checkpoint on `e55e5053`: native full target build/install passed,
focused CTest 22/22 and eight GPU selectors 8/8 passed. Evidence:
`merge-table-final-build-20260927.log`, `merge-table-final-ctest-20260927.log`,
`merge-gpu-*-table-final-20260927.txt`. Original game retry
`yotei-integrated-20260927-202436-menucheck-gpuav-sync` passes the two native
integration crashes but stops at the explicit bounded mode-0 zero-stride vector
candidate boundary (exit 321, shown=0). Next required regression is numerical
selection between normal and zero-stride candidates, formatted defaults/D16
and sparse EXEC, before shared candidate-specific lowering. Menu remains pending.

## Restored RO aligned-upload regression source (2026-09-27)

The historical RED/GREEN logs existed but the selector implementation had been
lost from the worktree. Restored `--storage-buffer-ro-aligned-upload-only` now
checks the real renderer binding and copies its upload bytes back from Vulkan.
The unchanged fixture fails with `HEAD` descriptors at guest offset 2
(`_Build/logs/ro-align-restored-red-20260927.txt`, exit 321, unsupported rebase)
and passes with the preserved WIP implementation
(`ro-align-restored-green-20260927.txt`, exit 0). Offsets 1/2/3 and zero padding
are covered; neighboring byte-offset binding and rejection tests pass. Pending:
independent GPU-dirty read-only negative fixture. A hash-only cross-ProgramKey
SPIR-V reuse experiment was removed before commit because it omitted binary
equality; the existing collision-checked sibling permutation reuse remains.

## Runtime sampler binding in synthetic GPU cases (2026-09-27)

Existing `ImageSampleR128DynamicMaterialPairs` numerical RED:
`_Build/runs/synthetic-image-20260927-routing/00000/stderr.txt` expects
`[80,8,20,2]` but receives `[80,8,80,8]`. The harness ignores its
`use_runtime_samplers` flag and binds a single ClampToEdge sampler. Before
using these fixtures as emulator evidence, bind each materialized sampler
through the runtime sampler cache and rerun the unchanged numerical oracle.
Static sampler coverage must remain green; no production sampler change is
justified by this harness failure.

GREEN: `_Build/runs/synthetic-image-20260927-runtime-samplers-green/report.json`
passes both unchanged numerical fixtures (2/2) with runtime samplers bound.

## Graphics wave64 emitter routing on subgroup32 (2026-09-27)

Before changing collective emission, prove a CPU SPIR-V regression for the
existing graphics partition contract: a Vertex wave64 module on native32 must
route ballot through the shared partition helper and normalize shuffle targets
to native lanes. Cover Pixel too and leave compute/dual-context wave64 paths
unchanged. This restores existing partition behavior; it does not establish
true cross-subgroup graphics wave64 equivalence. The captured vertex mask
clearing loop suggests this missing routing can prevent termination; that
runtime attribution requires a bounded game retry and source/menu evidence.

Confirmed CPU RED: `_Build/logs/graphics-wave64-red-20260927.txt.stderr`
reports `graphics wave64 shuffle target bypassed native32 normalization`.
The unchanged selector passes after restoring shared helper routing:
`graphics-wave64-green-20260927.txt` (native Windows, exit 0).
Neighboring partitioned graphics loop, cooperative collective functions and
split-wave64 swizzle/ballot selectors pass. Game attribution remains pending.

## Homogeneous indirect image sampling: opaque SSA validation (2026-09-27)

Required regression before correcting the uncommitted sampling optimization:
`shader_recompiler_compute_tests --homogeneous-indirect-image-validation-only`.
Two independent same-dimension float image candidates must emit valid Vulkan
SPIR-V and preserve candidate selection. `OpSampledImage` results cannot be
merged through `OpPhi`; sample in each branch and merge numerical results.
The CPU selector validates SPIR-V without initializing the GPU. Keep existing
heterogeneous dimension/numeric coverage and require GPU candidate readback
before claiming the 250-candidate game draw is fixed. Menu remains pending.

This file records regression coverage deferred during fast launch bring-up. Each item
describes a guest contract rather than a title-specific workaround. Deferred tests must
be added before the corresponding fixes are proposed upstream.

## Workgroup-axis bounded SRT pipeline identity

Status: GREEN materialization contract (26 September 2026); game still sees
remaining `SpecializationMiss` from **non-workgroup** columns.

Game evidence `…-212219-presentprobe-gpuav` (LOGF tip): CS `54904fb419d79e49`
`SpecializationMiss` ×3 with `bounded_srt[2..4]` counts `16→14→19→11` (and
matching flat-offset shifts). Columns `[0],[1]` stay `count=1`; `[5],[6]` stay
`0`. `ProgramKeySplit=0` for this hash. Workgroup-axis reserve is therefore
insufficient alone — selector/count-source layout counts still splinter
CreatePipeline identity (cold GPUAV ≈270s/variant; warm cache hides cost).

Contract (extended): count-source / selector-sourced bounded columns need a
stable layout upper bound (or non-module-affecting count) so dispatch-varying
live sizes do not create new SPIR-V, matching the workgroup-axis reserve
rule. Live words still fill only the used prefix.

GREEN (workgroup only): `resource_tracking_tests --workgroup-srt-materialization-only`
→ `KYTY_WORKGROUP_SRT_MATERIALIZATION_PASS`. Selector-stable reserve: RED pending.

## VS indirect-image expansion DeviceLost (~shown=191)

Status: failing draw identified; cause not established. The attempted opaque
SSA optimization failed CPU validation on 27 September and was removed.

SyncDiag + GPUAV8: DeviceLost on first draw using VS `0xe3125617f3efc38f`
(guest ES `0x803f946a00`) with PS `0x803fe78e00`, indexed count=119856.
SpecializationCompile: `images=251 sampled_pairs=250`,
`indirect_search_iterations=9`. Neighboring VS specialize with `images=0`.
Prior same-PS draws with ES `0x803fba0000` complete. Menu blocked here.

Read-only SPIR-V inspection on 2026-10-03: the VS loop selects a lane from
two 32-bit masks, normalizes the guest target to native 0..31, shuffles,
then uses `OpGroupNonUniformBallot` whose lower 32-bit word is duplicated
into both guest words. The duplicated words clear both loop masks. This is
the documented partial native32 graphics wave64 model; static inspection
does not prove the loop hangs in the observed draw. The existing 256-iteration
diagnostic budget applies to Pixel only, not Vertex. Before attributing
DeviceLost to this loop, add a bounded synthetic Vertex case with
independently expected high/low masks and a completion signal. A silent
vertex-loop cap would discard guest work and cannot be shipped as a fix.

Contract: preserve candidate selection and valid Vulkan SPIR-V. Opaque
`OpSampledImage` values cannot be merged by `OpPhi` or consumed across blocks.
The existing per-candidate sampling switch remains until a legal alternative
has independent validation and numerical GPU coverage. The CPU-only
`--homogeneous-indirect-image-validation-only` proves the attempted variant
invalid and passes with numerical result merging restored. Logs:
`_Build/logs/homogeneous-image-red2-20260927.txt.stderr` and
`homogeneous-image-green2-20260927.txt`. This does not prove the draw fixed.

## Current Windows regression status after branch reconciliation

Status at source revision `9a29c306` (25 September 2026): native Windows
`launcher` + `kyty_tests` build passed. The three required Windows CI tests
and three neighboring memory/kernel/resource tests passed 6/6. Logs are in
`_Build/merge-validation-20260925/final-full-build.*` and `final-tests.*`.
The full registered 55-test suite was not run; do not carry forward the old
46/51 result as a current result.

The bounded extended CPU run (`extended-cpu-tests.*`) passed 1/3:

- `scalar_provenance` now passes.
- `shader_cfg` passes the earlier wide-buffer budget check and the restored
  SAVEEXEC mask operation, then fails `nested-tail routing changed semantic
  instruction coverage`. This test and CFG implementation already existed at
  the pre-integration branch head; the newly reached failure needs its own
  unchanged regression and CFG-level correction.
- `resource_tracking` still fails because the invariant indirect-image proof
  admits a wrapped scalar immediate. This failure was recorded before the
  current integration.

Additional resource selectors `finite-selector-srt-proof-only`,
`finite-selector-srt-materialization-only` and
`workgroup-srt-materialization-only` fail. Their logs are under
`_Build/merge-validation-20260925/resource7-*`. The finite selector checks
expose missing rejection of unproved selection and writable descriptor-source
overlap; the workgroup check exposes an exceeded combined probe budget. These
results are not included in the 1/3 CTest count and need independent fixes.
The earlier `shader_recompiler_compute` and `texture_cache_image_overlap`
sampled-alias failures were not rechecked on this revision.

For history, the 9 September merge `a309653` of `upstream/main` at `0b4e78c`
had 46/51 extended CTest passes, with wide-buffer, scalar-provenance,
resource-tracking and two sampled-alias failures. Keep historical and current
results separate when judging launch readiness.

## Cooperative wave64 read-only LDS phase batching

Status: CPU regression added and Vulkan neighboring coverage complete; large-module
driver compile remains open.

Observed trigger: the cooperative emitter ended every LDS instruction with a full
workgroup rendezvous. Consecutive read-only accesses therefore created redundant phases,
guards and cross-phase spills even though no invocation could publish a conflicting LDS
write between them. The exact large compute module shrank from 702,611 to 697,920 words,
but NVIDIA `vkCreateComputePipelines` still exceeded the 60-second frame watchdog.

Covered tests:

- One and two consecutive read-only LDS accesses emit the same number of
  `OpControlBarrier` instructions.
- An intervening LDS write retains both required read/write hazard boundaries.
- Cooperative SSBO, cyclic scalar/physical address and BDA coefficient Vulkan tests
  execute successfully on the native Windows GPU path.

Remaining debt:

- Factor repeated split-wave64 ballot/readlane lowering into reusable SPIR-V functions
  without changing workgroup scratch, dynamic-uniformity or barrier semantics.
- Ratchet exact synthetic module size and validate that bounded pipeline creation no
  longer exceeds several minutes under GPUAV instrumentation before claiming menu
  progress. Game evidence 26.09 `…-081840`: CS `54904fb419d79e49` did **4×**
  specialization CreatePipeline ≈290/295/287 s under GPUAV+instr; watchdog
  shown=159 before spinner readback.

## Compressed video-out metadata on a native render-target alias

Status: regression RED confirmed on Windows 2026-09-28.
`--buffer-cache-range-only` fails at "video-out metadata on render-target alias"
with merge 8e61798b and identically with only the two upstream textureCache files
replaced by HEAD 45e09731 versions. Logs `merge-8e-image-ownership-20260928` and
`merge-8e-image-ownership-baseline-20260928` (bounded, no timeout). New upstream
ownership cases later in the fixture are not reached. Shared metadata fix pending.

Observed trigger: a guest color target is first discovered as a native render target
with DCC metadata, then the same allocation is acquired through VideoOut with an
explicit compressed-surface classification. `TextureCache::FindImage` correctly
reuses the native image, but the requested VideoOut metadata must not be lost when
the cache record was created by the render-target path.

Required tests:

- A render-target discovery followed by a compressed VideoOut lookup for the same
  backing must reuse the native image and retain the VideoOut compression/control
  metadata needed by presentation.
- An uncompressed VideoOut lookup must not erase DCC metadata already registered
  for a native render target.
- The alias must remain GPU-owned and must not trigger a guest upload or compressed
  readback while preparing the frame.

## Runtime descriptor-evaluation failure diagnostics

Status: production diagnostic and native game validation complete; automated regression deferred.

Observed trigger: a descriptor source can pass the static runtime-value validator and still
fail when evaluated against the current dispatch state. The old message identified only the
source and DWORD, hiding whether the cause was a short user-data span, a cyclic value, an
unsupported opcode, invalid memory metadata, an out-of-range scalar-buffer read or unavailable
guest memory. This made a generic correction indistinguishable from a transient dirty-resource
retry.

Required tests:

- Immediate, user-data and shader-base sources proving that successful evaluation remains silent
  and a user-data index outside the current runtime span reports both the requested and available
  ranges.
- Unsupported opcode, undefined value, malformed composite extraction and cyclic dependency
  cases proving that the deepest failing opcode/reason is retained in the source/DWORD diagnostic.
- Scalar-address and scalar-buffer reads covering invalid metadata, failed address arguments,
  negative/out-of-range offsets and unavailable guest memory, with the computed address included
  where one exists.
- `ReadFirstLane` and clean-snapshot delegation cases proving that a nested evaluator's reason is
  propagated through the outer descriptor source rather than replaced by a generic failure.
- Transactional materialization checks proving that detailed diagnostics do not mutate the
  previous `ResourceSnapshot` or `ResourceSpecialization` on failure.

## Bounded descriptor columns behind flattened SRT slots

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: `BuildSrtPlan` can replace a scalar descriptor-table load with `ReadConst`,
while the saved flat slot still resolves to a read that resource tracking later converts to
`ReadBoundedSrtU32`. Bounded buffer/image recognition looked only at the immediate handle argument,
so it missed four/eight correlated columns hidden behind those flat slots. Ordinary runtime
materialization then tried to evaluate the GPU-selected bounded value as one invariant descriptor.

Required tests:

- Four correlated buffer-descriptor columns passed directly as bounded reads and through four
  distinct `ReadConst` flat slots, checking that both forms produce the same dense candidate table.
- Eight correlated image-descriptor columns behind flattened slots, checking descriptor
  deduplication, key mapping and null/invalid descriptor normalization.
- Mixed direct/flattened columns, nested flat slots and shared flat slots proving recognition
  follows only exact `SrtRead` values, rejects a slot cycle and preserves the live GPU selector.
- Rejection cases for an invalid flat index, a non-immediate slot, unrelated bounded reads,
  mismatched count/address/stride/bias, partial descriptors and workgroup-indexed tables.
- Materialization and SPIR-V checks proving the flattened snapshot remains immutable, all column
  reads share one coherent transaction, and the generated resource-table lookup is bounds safe.

## Dependent buffer, image and sampler descriptors addressed by bounded SRT values

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: a buffer, image or sampler descriptor can be loaded through `LoadAddressU32`
whose address itself comes from one or more `ReadBoundedSrtU32` values. The bounded values are
GPU-selected, so ordinary host evaluation cannot produce one invariant descriptor; direct
four/eight-column table recognition also does not apply because the bounded read is an address
dependency rather than a descriptor DWORD.

Required tests:

- A bounded low/high pointer pair feeding four dependent `LoadAddressU32` descriptor words,
  checking that materialization evaluates every proved selector candidate and preserves the live
  selector in generated SPIR-V.
- The equivalent eight-word image descriptor, including invalid/null normalization, typed
  candidate specialization and sampled/storage binding selection.
- A four-word sampler expression paired with an image expression using the same proved selector,
  checking that every dense image candidate receives the matching sampler descriptor and depth
  comparison function.
- Repeated image descriptors with different sampler descriptors, proving candidate deduplication
  uses the image/sampler pair and does not silently collapse distinct filtering state.
- Candidate deduplication, zero-count and repeated-address cases, checking dense buffer IDs and the
  flattened selector mapping against an independent oracle.
- A bounded scalar-buffer dependency whose final candidate is outside the descriptor byte extent,
  checking that it snapshots the guest buffer-load zero result while an unavailable in-range host
  read still fails the transaction.
- Dependent descriptor evaluation through scalar-buffer and raw-address reads, checking that an
  out-of-bounds scalar-buffer word and a null-base raw read in an over-approximated bounded
  candidate produce zero, while every non-null unavailable or GPU-dirty address still fails.
- Multiple bounded dependencies with the same key/count but different source tables, and nested
  runtime-uniform address arithmetic, proving one coherent snapshot transaction covers all reads.
- Several descriptor columns sharing one 16-bit selector domain, proving the 65,536-candidate
  limit applies to each read while a separate bounded total-word budget covers the coherent
  snapshot; also check that exceeding either limit fails before any partial snapshot escapes.
- Flattened `ReadConst` slots wrapping GPU-selected address dependencies, checking that the base
  snapshot reserves those slots without eagerly evaluating them and candidate evaluation still
  follows the original saved value graph.
- Rejection cases for different selector values, counts, signedness, workgroup axes, invalid read
  IDs, dependency cycles, unavailable/dirty descriptor memory and descriptor/resource limits.
- A bounded sampler used without a correlatable bounded image, and an image referenced with two
  different bounded sampler tables, proving unsupported independent sampler selection fails closed.
- Writable-alias and transactionality cases proving no eager snapshot overlaps a writable guest
  range and no partial candidates escape after any dependent read fails.

## Wave64 uniformity through bounded resource selection

Status: production fix, offline manifest audit and native game validation complete; automated
regression deferred.

Observed trigger: a split-wave64 compute shader reaches a conditional branch after its
buffer/image/sampler resources have been specialized from a bounded selector. The execution
planner rejects the branch at guest PC 612 because the current greatest-fixed-point analysis
classifies at least one value in the condition as lane-varying without reporting the dependency
that caused the classification.

Required tests:

- A branch whose condition depends on each runtime resource-selection opcode that is emitted once
  per guest wave, proving only values with an explicit wave-uniform execution contract are accepted.
- A scalar `LoadAddressU32` branch with a uniform handle/offset and matching `ScalarAddress`
  metadata, plus varying-handle, varying-offset, missing-metadata and non-scalar-address rejection
  cases.
- The corresponding per-lane buffer/image result and lane/subgroup-derived selectors, proving a
  bounded descriptor table alone never makes the resource operation's returned data uniform.
- Nested `ReadConst`, `ReadBoundedSrtU32`, scalar-address arithmetic, comparisons, selects and Phi
  nodes, checking that uniformity propagates through pure operations and loop-carried values only
  when every reachable input and control edge is uniform.
- A diagnostic case that reports the first varying opcode/value chain and guest PC for a rejected
  branch, so corpus failures can be grouped without weakening the convergence rule.
- Split native32, native64 and cooperative multi-wave execution coverage, with a collective inside
  the conditional arm to prove accepted branches keep both native halves at matching rendezvous.

## Guard-bounded inline sampled tables

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: a material shader multiplies a dynamically reduced selector by a 368-byte record
stride, but a dominating unsigned branch admits descriptor loads only while the selector is below
255. The old materializer discarded that CFG fact and enumerated the full wrapped-U32 domain:
1,445 in-buffer offsets at 16-byte GCD spacing, exhausting the candidate limit on records the shader
cannot access. Invalid images also retained unrelated adjacent sampler words as artificial pairs.
After preserving the guard, the table has 24 candidates; combined with 115 direct images it needs
138 dense image slots. A second static-state/runtime permutation of the same shader needs 285 dense
images. The compiler image and sampled-pair ceilings are raised to 512 while sampler capacity
remains 128 and final renderer admission continues to enforce the physical Vulkan descriptor limits.

Required tests:

- Direct image and sampler tables under equivalent dominating `selector < limit` and
  `selector >= limit` guards, proving only the bounded branch records the exclusive limit.
- Nested and loop-carried guarded selectors, including a path that bypasses the guard and an
  opposite branch that can re-enter through a loop, proving the bound is accepted only when the
  guarded edge is the sole immediate region containing the descriptor handle.
- A selector without a proven guard, proving materialization retains full wrapped-U32/GCD
  enumeration and its existing probe limit.
- A wrapped selector that visits valid records and misaligned words, proving every invalid image is
  paired with the canonical null sampler while valid image/sampler combinations stay distinct.
- Multiple different sampler bit patterns beside null images, proving they map to one candidate and
  do not consume sampler origins, point-sampler clones or sampled-pair slots.
- Null and valid image candidates sharing the same selector, checking exact key-to-candidate mapping,
  emitted switch cases and a native sampled readback for both paths.
- A genuinely bounded table with 129 distinct compatible image/sampler pairs, proving the unchanged
  sampler and sampled-pair ceilings still reject real resource pressure transactionally.
- Shaders with 138 and 285 total direct and indirect images, plus a 513th-image/pair rejection,
  checking dense remapping, bounded compiler work and physical Vulkan descriptor-budget validation.
- The captured shader with selector limit 255 and stride 368, followed by Vulkan and SPIR-V
  validation and a game run that advances past its former 129th artificial pair.

## Null optional scalar-address descriptor roots

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: eager host materialization of a vertex shader descriptor source evaluates four
`LoadAddressU32` words from an optional SRT pointer whose exact 48-bit base is zero. Adding the
descriptor's first offset produces guest address `0x10`, which is intentionally unmapped; the
optional branch should specialize to a null descriptor without weakening failures for non-null
unreadable or GPU-dirty memory.

Required tests:

- A four-word buffer descriptor source loaded from an exact null `GetAddressResource` base with
  positive immediate offsets, proving every word materializes as zero.
- Null bases with zero, positive and negative scalar/immediate offsets, proving pointer arithmetic
  is not performed after the null root is established.
- A non-null readable base, proving ordinary descriptor words remain unchanged.
- A non-null unreadable base and a non-null GPU-dirty specialization read, proving both still fail
  closed with the exact guest address.
- A guarded runtime path where the optional descriptor is unused when null and used when present,
  checking null binding behavior and the captured `ee4f153aa500d327` vertex shader.

Validation compiled `ee4f153aa500d327` through resource tracking and emitted 15,847 SPIR-V words;
the game then advanced to renderer descriptor binding and failed at the next independent storage
buffer offset boundary. Exact null roots specialize to zero before pointer arithmetic, while
non-null unreadable and GPU-dirty roots retain the existing fail-closed path.

## Storage-buffer backing offsets beyond the packed residual ABI

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: after the optional null SRT shader compiles, renderer binding obtains a guest
storage-buffer subrange whose offset inside a shared Vulkan backing cannot be represented by the
current one-byte residual field or violates the access-shape admission rules. The current fatal
does not print the guest address, backing offset, device alignment, residual, range or resource
access facts, so the exact boundary must be measured before selecting a representation.

Required tests:

- Shared backings with residual offsets at 0, 1, 3, 4, 255, 256 and the device alignment minus
  one, checking the descriptor offset, bound range and shader-visible effective byte address.
- Raw, scalar, typed, formatted 8-bit and formatted 16-bit reads and writes at aligned and
  unaligned guest addresses, including accesses spanning two native DWORDs.
- A residual wider than the packed one-byte shader-data field, proving it is either rebased into
  a dedicated buffer view or represented by a wider ABI without truncation.
- Exact-end, partial-DWORD-tail, maximum Vulkan range and arithmetic-overflow cases, checking that
  out-of-range guest bytes cannot become visible through alignment expansion.
- Read/write and atomic resources backed by an existing larger allocation, checking cache
  invalidation and aliasing after any rebase or dedicated-view fallback.
- Vulkan validation/readback plus a bounded game run past the renderer failure following shader
  `ee4f153aa500d327`.

Diagnosis measured vertex slot 6 at guest address `0x80760a1a16`, size `0x240`, Vulkan alignment
16 and residual 6. The resource is descriptor-formatted-only, read-only and non-atomic. The new
shader-data layout carries an exact byte limit per buffer, so the Vulkan range may be rounded to a
complete DWORD without exposing padding to guest accesses; 16-bit cross-DWORD loads and stores
join or split their two backing words. Both native targets build. Cache-warming runs have reached
shader 131 without fatal or validation output. The final stable-SHA validation run compiled all
142 shaders, accepted the slot-6 binding and advanced to the next independent graphics-stage
interface VUID.

## Missing producer declarations for consumed graphics-stage parameters

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: a fragment shader declares a per-vertex `array[3] of vec4` input at SPIR-V
Location 2, while the paired vertex shader does not declare an output at that location. Both
modules validate independently, but `vkCreateGraphicsPipelines` rejects their combined interface
with VUID `RuntimeSpirv-OpEntryPoint-08743`.

Required tests:

- A pixel parameter consumed at a location exported by the vertex shader, proving no duplicate
  output declaration is added and the original value remains connected.
- A consumed pixel location absent from vertex exports, proving the producer declares a matching
  `vec4` output and the graphics pipeline passes interface validation.
- Per-vertex barycentric fragment inputs, including the three-element fragment array contract,
  matched against the preceding vertex stage's non-array output declaration.
- Sparse locations, flat and no-perspective inputs, repeated guest interpolator mappings and
  collision-remapped host locations, with deterministic producer declarations for every consumer.
- Vertex-program cache variants paired with different fragment input masks, proving the interface
  requirement participates in the static key and cannot reuse an incompatible module.
- Host `maxVertexOutputComponents` admission and a bounded validation run beyond the pipeline pair
  containing vertex shader `ee4f153aa500d327`.

The program cache now compiles the active pixel stage first and carries its exact guest-source to
host-location parameter links into the paired vertex static key. Vertex compilation keeps matching
exports, duplicates a produced value when collision remapping needs an alias, removes an
unconsumed output that occupies a required location, and declares an unwritten synthetic output
when the guest producer is absent. A native Windows build of both targets completed. Diagnostic
run `yotei-integrated-20260907-051751-146531` advanced from shader 142 to 158 without the previous
Location 2 VUID or another validation/fatal message.

## Shared descriptor dependency DAG traversal

Status: production fix and exact captured-shader performance validation complete; bounded game
validation and automated regression deferred.

Observed trigger: captured compute shader `7ceb0f3417f926f9` reaches resource tracking with 3207
normalized IR instructions, then spends more than 75 seconds in `Collect` for one
`ImageSampleRaw`. The image and sampler descriptor values contain a large Phi/Select DAG with
shared subgraphs. `CollectBoundedDependencies` remembers only the current recursion stack, so a
valid acyclic subgraph is revisited once per incoming path and traversal grows exponentially.

Required tests:

- A diamond-shaped acyclic descriptor expression with exponentially many paths but linearly many
  IR nodes, proving each completed node is traversed once for one descriptor.
- Shared subgraphs across all image and sampler descriptor DWORDs, proving the completed-node set
  is shared while every distinct bounded read is retained exactly once.
- A real dependency cycle and an invalid `ReadConst` flat slot, proving both still fail closed and
  cannot be hidden by completed-node memoization.
- Two branches reaching different bounded reads below a shared parent, proving memoization does not
  discard either dependency or merge unrelated selector/count groups.
- Native Windows audit of captured `compute_7ceb0f3417f926f9_7847dd7220f76d66.json` under a strict
  timeout, followed by a bounded game run beyond that compute shader.

Current evidence: completed-node memoization is shared across every DWORD of one descriptor while
the recursion-stack set still rejects cycles. The native Windows audit now reaches the next genuine
semantic rejection in 114 ms; the same shader previously remained inside `Collect` for more than
75 seconds before it was stopped. The next rejection is an unevaluable `ReadFirstLane` image
descriptor word and is tracked separately from this traversal-performance defect.

## Wave-uniform image descriptor candidate selection

Status: production fix, native build, exact captured-shader audit and runtime game validation
complete; automated regression deferred.

Observed trigger: compute shader `7ceb0f3417f926f9` builds eight correlated image-descriptor
DWORDs through matching `Phi` and `SelectU32` graphs, broadcasts every selected DWORD with
`ReadFirstLane`, and samples the resulting wave-uniform descriptor at guest PC `0x0000066c`.
The correlated graph has four host-readable descriptor candidates plus a null candidate, but a
single descriptor source cannot currently retain and materialize that finite candidate set.

Required tests:

- Eight `ReadFirstLane` roots with one active mask and isomorphic `Phi`/`SelectU32` topology,
  proving correlated `ReadConst` leaves become complete descriptor candidates rather than a
  Cartesian product of independent DWORDs.
- Mixed immediate and flat-SRT candidate DWORDs, repeated candidates, and the all-zero null
  candidate, proving materialization preserves exact descriptor tuples and deduplicates safely.
- Mismatched active masks, control topology, Phi predecessors, descriptor widths, invalid flat
  slots, cyclic tuples, and more candidates than the dense image limit, proving recognition fails
  closed.
- Two different descriptors with the same selected key DWORD, proving specialization rejects the
  ambiguous mapping instead of silently binding the wrong image.
- Native Windows audit of captured `compute_7ceb0f3417f926f9_7847dd7220f76d66.json`, followed by a
  bounded game run past guest PC `0x0000066c` with Vulkan validation enabled.

Current evidence: the shared recognizer accepts only eight `ReadFirstLane` roots with the same
active mask and matching `Phi`/`SelectU32` topology. It extracted four host-readable descriptor
tuples plus the null tuple from the captured shader. Both native Windows targets build, and the
exact shader now passes resource tracking and compute-execution precheck in about 0.27 seconds.
The bounded game run materialized all five candidates, emitted 103,404 SPIR-V words, compiled the
shader as number 159 and advanced through six more shaders before the separate GPU draw fault.

## GPU command completion fault isolation

Status: production diagnostics and native game validation complete; dispatch-filter and draw
eligibility regression added; end-to-end GPU fault-isolation coverage remains deferred.

Observed trigger: after `7ceb0f3417f926f9` successfully materializes, emits 103,404 SPIR-V
words, and compiles as shader 159, the game compiles six more shaders and the NVIDIA driver emits
Windows System event `nvlddmkm` 153 for `\\Device\\Video3`. The emulator then receives
`UINT64_MAX` from `vkGetSemaphoreCounterValue` at scheduler tick 16720 and exits with code 321.
That sentinel follows a real GPU driver fault; the semaphore invariant correctly prevents it from
marking every pending resource free.

Current evidence: a filtered synchronized run proved guest compute address `0x8000383f00` with
grid `128x128x1` completes in 321,278 us. The same run later reproduced `nvlddmkm` 153 and the
invalid timeline sentinel at tick 16718, so this dispatch is not the command that hangs the GPU.
Synchronizing all compute and draw calls then isolated the reset to an auto draw with four
vertices and one instance: pixel shader address `0x80003cbb00`, export/vertex shader address
`0x8000408e00`, submit 636. Its preceding `65x65x1` compute dispatch completed in 7,824 us;
the draw was recorded, but its completion wait produced the driver reset.

Required tests:

- Synchronized dispatch diagnostics around every non-empty guest dispatch, with strict optional
  minimum-workgroup and exact-grid filters. Valid filters must isolate matching commands without
  changing their execution; malformed filters must disable the diagnostic instead of silently
  synchronizing every dispatch. The trace must prove the exact shader address, grid, local size,
  and submit ID whose completion triggers the driver fault.
- Synchronized indexed and auto-draw diagnostics with vertex/pixel shader addresses, primitive
  counts, instance counts and submit IDs. Before/after completion markers must distinguish a
  failing draw from queued work that only happens to be observed at the next compute wait.
- Focused `--gpu-sync-diagnostic-only` coverage must keep non-empty draws traceable when either
  dispatch-only filter is set, while retaining minimum/exact matching for dispatches and rejecting
  empty draws or invalid configurations. This regression is now GREEN on the native Windows test
  executable.
- A large finite dispatch split into base-workgroup tiles, proving every guest workgroup executes
  exactly once and `WorkgroupId`/`GlobalInvocationId` retain the unsplit values.
- Storage/image writes shared across tiles, proving submission boundaries preserve the original
  dispatch's read/write ordering and descriptor lifetime.
- Zero dimensions, thread-dimension mode, partitioned wave64 grids, and device workgroup limits,
  proving tiling neither invents work nor exceeds Vulkan limits.
- Native Windows game run with validation plus a System-event check, proving the target dispatch
  completes without `nvlddmkm` 153 and execution advances beyond the current shader frontier.

## Graphics wave64 on native subgroup32

Status: production fix, native build, SPIR-V validation and game validation complete; automated
regression deferred.

Observed trigger: a wave64 pixel shader can be emitted for a host whose graphics stages execute
with native subgroup size 32. The captured shader `964747887d898821` contains subgroup shuffles
to guest lanes 31 and 63, and the lane-63 value controls a structured loop exit. Passing 63
directly to `OpGroupNonUniformShuffle` is outside the native subgroup and leaves the result
undefined. The corresponding four-vertex auto draw returns from recording but resets the NVIDIA
driver while the renderer waits for completion.

Exact cross-half exchange cannot be reconstructed with workgroup scratch and barriers in a
fragment stage. The safe graphics fallback therefore has to define a coherent partitioned
wave64 model: each native subgroup32 executes one guest half, guest lane selectors address the
corresponding local lane, and native ballot bits are reflected into both 32-bit guest words. This
keeps subgroup operations finite and deterministic while retaining an explicit correctness debt
for values that truly differ between the two guest halves.

Required tests:

- Pixel and vertex wave64 compilation with a native subgroup32 profile, proving every constant
  and dynamic `ReadLane`/DPP shuffle index supplied to SPIR-V is within 0...31.
- Ballot, `EXEC`/`VCC`, WQM, find-first and lane-active cases proving both guest mask words use the
  same native-half predicate and all derived bit tests remain internally consistent.
- A bounded graphics loop whose exit reads guest lanes 31 and 63, proving the emitted module
  validates and terminates on a native subgroup32 device instead of causing a watchdog reset.
- A partitioned pixel loop that stops normally on iteration 255 and one that never makes
  progress, proving the safety counter preserves the valid exit and terminates the latter
  fragment after at most 256 body iterations. Multiple and nested loops must share a documented
  per-invocation budget without creating invalid SPIR-V edges or broken Phi dominance.
- Wave32, native subgroup64 and compute cooperative-wave64 cases proving the partitioned graphics
  fallback does not alter their SPIR-V or execution plan.
- Cross-half values that intentionally differ, documenting the approximation and proving the
  emulator rejects or selects a future exact implementation before claiming pixel correctness.
- Native Windows replay of the captured pixel/vertex pair followed by a game run, draw completion
  markers and a System-event query proving the failing auto draw completes without a new
  `nvlddmkm` 153 event.

Current evidence: the emitted pixel module for `964747887d898821` passes `spirv-val` with the
loop counter inserted at the direct loop-body entry after Phi nodes. In native run
`yotei-integrated-20260907-073053-841d50`, the formerly failing four-vertex auto draw produced
`after-complete`; later draw and dispatch commands completed, no new `nvlddmkm` event appeared,
and execution advanced from shader 164 through shader 171. The next failure is an independent
CPU decoder rejection of GFX10 `MUBUF opcode 0x20` in compute shader `d7a83911714a58ee`.
This evidence proves the watchdog failure is cleared for the captured path; it does not prove
exact guest cross-half values or non-black pixel output.

## GFX10 byte-to-D16 buffer loads

Status: production fix, native build, exact shader audit and bounded game validation complete;
automated regression deferred.

Observed trigger: compute shader `d7a83911714a58ee` reaches a GFX10 MUBUF instruction with raw
words `[0xe080e000 0x80040003]` at guest PC `0x60`. The current decoder rejects opcode `0x20`.
Independent GFX1030 disassembly identifies it as `buffer_load_ubyte_d16`; the adjacent opcodes
`0x21...0x23` are the high-half unsigned form and the low/high signed-byte forms.

Required tests:

- Decode exact GFX10 MUBUF opcodes `0x20...0x23` to unsigned/signed byte-to-D16 low/high
  operations with one destination DWORD, eight memory bits, and the correct destination-half
  selector. Existing non-D16 byte and short opcodes must retain their current metadata.
- Unsigned byte values `0x00`, `0x7f`, `0x80`, and `0xff`, proving zero extension to the selected
  16-bit half while the other half of VDATA is preserved exactly.
- Signed byte values at the same boundaries, proving sign extension to 16 bits before insertion
  and preservation of the unselected half for both low and high forms.
- Indexed, offset, scalar-offset, combined-address and out-of-bounds reads, proving the new forms
  reuse the shared buffer address and bounds path and replace only their selected half.
- Decode rejection around reserved neighboring encodings plus replay of the exact captured
  shader, proving support is based on the ISA opcode family rather than a title or shader hash.
- Native Windows game run proving `d7a83911714a58ee` advances beyond CFG, translation, SPIR-V
  validation and pipeline creation without regressing the preceding graphics-wave64 draw.

Current evidence: native `shader_cfg_tests --audit-shader` decodes all 55 instructions and
passes the exact capture through CFG, IR translation and resource tracking. In game run
`yotei-integrated-20260907-075324-3c906f`, `d7a83911714a58ee` emitted 6,537 SPIR-V words,
compiled as shader 172, and its 76x1x1 dispatch completed in 63,618 us. Execution then compiled
two more shaders and stopped independently in resource tracking for `6cc64dee32dc7094`.

## Entry-prefix bounded SRT tables with dispatcher CFG fallback

Status: production proof fix implemented and validated through the next captured/runtime
resource-tracking boundary; automated regression and full materialization validation deferred.

Observed trigger: compute shader `6cc64dee32dc7094` uses a five-bit unsigned extraction to form
the byte offset `0x1030 + selector * 16`, then reads four descriptor DWORDs from the root SRT in
an unconditional entry prefix that IR represents as several basic blocks. Its CFG requires the
general dispatcher emitter because two headers share a structured merge. The bounded SRT proof
previously rejected every dispatcher program before considering that this finite table read
occurs before any control-flow split, so resource tracking later rejected descriptor DWORD 0
with root `LoadAddressU32` at PC `0x530`.

Required tests:

- A dispatcher-fallback compute program whose single-entry, unconditional block chain contains a
  five-bit selector and four correlated SRT reads at stride 16, proving exactly 32 coherent
  descriptor candidates are materialized while the live selector remains on the GPU.
- Entry-prefix ordering cases proving the selector definition and all raw descriptor reads follow
  only unconditional edges with one predecessor and precede the consuming handle; a read after a
  control-flow split, in a conditional block, or with a non-dominating address root must remain
  rejected.
- Phi, loop-carried, workgroup-indexed and non-finite selectors in a dispatcher program, proving
  the narrow admission rule cannot inherit structured-loop or cross-block assumptions.
- Address overflow, width 0/32, mismatched columns, non-consecutive offsets, unreadable memory,
  snapshot retry, candidate and byte-budget boundaries, all failing closed without partial
  descriptor publication.
- A structured version of the same table and unrelated dispatcher shaders, proving the existing
  bounded proofs and generic switch emitter retain their current behavior.
- Exact captured-shader audit followed by native game validation. The current implementation
  removes the PC `0x530` buffer-descriptor failure in both and reaches the independent image
  descriptor at PC `0x7c4`; resource tracking, materialization, SPIR-V validation and the
  4096x1x1 dispatch remain pending until that next blocker is fixed.

## Periodic Vulkan pipeline-cache checkpoints

Status: production fix and two-run native game validation complete; automated regression
deferred.

Observed trigger: bounded emulator runs often end by externally terminating the process
after the window does not close within its grace period. The Vulkan driver cache was only
serialized during normal renderer destruction, so every forced stop discarded all pipelines
compiled since startup and made the next run repeat the same expensive work.

Required tests:

- A cache-lifecycle case that creates enough new graphics and compute pipelines to cross the
  checkpoint threshold, then verifies that a complete signature, payload hash and driver blob
  are written without destroying the live `VkPipelineCache`.
- A continuation case that creates more pipelines after a checkpoint and proves the same live
  cache handle remains usable until final shutdown.
- Threshold boundary cases for zero, one less than the interval, the exact interval and several
  intervals, checking that cache hits do not count as newly created pipelines.
- A warm-cache case where newly created emulator pipeline objects leave the driver blob unchanged,
  checking that hash comparison suppresses redundant replacement of the on-disk cache.
- Disabled, empty and failed-driver-query cases proving that checkpoint attempts remain harmless
  and do not prevent later pipeline creation or final cleanup.
- File replacement and interrupted-write cases proving that a fully written temporary file is
  used and a truncated or mismatched cache cannot be accepted on the next launch.
- Concurrent graphics/compute creation coverage proving that serialization covers cache mutation
  and snapshot extraction, followed by two native Windows runs where the first is force-stopped
  and the second reports a compatible loaded cache.

## Bounded shader diagnostic dumps

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: file diagnostics serialize decoded ISA and native IR more than once for every
new shader. Large dispatcher programs can expand to tens of thousands or millions of IR
instructions; one bounded game run produced a 254 MiB log with 10,468,972 lines and reached its
timeout while formatting an early IR dump, before the next compiler phase could be observed.

Required tests:

- Small structured and dispatcher shaders below the instruction budget, checking that decoded ISA,
  pre-resource IR and final IR remain present and unchanged in diagnostic output.
- Exact-boundary and over-budget shaders, checking that expensive IR string construction is skipped
  and replaced by a compact message containing the actual instruction count and configured limit.
- A large dispatcher shader proving that normalization, resource tracking, specialization and
  SPIR-V emission still execute when verbose IR text is omitted.
- Phase-marker coverage around normalization/resource tracking so a timeout can be assigned to a
  compiler phase even when the detailed dump is suppressed.
- A silent guest-log run with `KYTY_SHADER_PHASE_TRACE=1`, checking that concise decode, CFG,
  normalization, resource-tracking and SPIR-V markers reach stdout without enabling the shared
  high-volume logger.
- Repeated permutations and mixed small/large shaders proving that each detailed dump applies the
  same bound and that concise phase tracing remains available for every program.

## Depth attachment rediscovery after resource preparation

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: draw setup discovers color/depth attachments before shader resources, vertex
buffers and descriptor bindings are prepared. That preparation can synchronously finish work and
replace an aliased cached image. Color attachments already rediscover and rebind their descriptor
at final acquisition, while an equally stale depth attachment terminated the emulator with
`depth target changed after render-state discovery`.

Required tests:

- A draw where sampled or storage resource preparation invalidates the initially discovered depth
  image, checking that final attachment acquisition finds and binds a replacement from the original
  depth descriptor.
- Registered, missing, unregistered and `needs_rebind` owner cases, including replacement with a
  different `ImageId` and reuse of a compatible existing depth image.
- Depth-only and mixed color/depth draws proving that extent, layer count, sample count, format,
  image view, pipeline rendering state and dynamic depth/stencil state use the final image.
- HTile and stencil cases proving rediscovery preserves metadata ownership, deferred clears,
  stencil association and the exact guest backing ranges.
- Negative cases for an actually incompatible replacement, invalid view or mixed sample count,
  checking that the existing precise validation remains after rediscovery.
- Native Vulkan validation with an alias transition between discovery and acquisition, followed by
  a bounded game run past the captured renderer invariant.

## Unsigned bitfield selectors for bounded resource tables

Status: production fix and corpus validation in progress; automated regression deferred.

Observed trigger: a four-word buffer descriptor is loaded from an immutable table using
an index produced by `BitFieldUExtract`. The source of the extracted bits can be
lane-varying, including a buffer lookup indexed by `WorkgroupId`, while an immediate
unsigned extraction still gives an exact dense range of `0..(2^width - 1)`.

Required tests:

- A compiler-level proof test where a 5-bit extraction drives four correlated descriptor
  reads with stride 16 and produces exactly 32 candidates.
- A materialization test that reads all correlated descriptor words coherently, deduplicates
  identical candidates, preserves the live GPU selector, and maps each key to the right buffer.
- Boundary cases for width 0, width 32, invalid `offset + width`, non-immediate offset/width,
  signed extraction, mismatched descriptor columns, and the existing resource/probe limits.
- A native Windows shader audit covering every captured manifest in the same normalized
  failure group, followed by a bounded Vulkan readback case before upstream submission.

## Uniform diamond Phi descriptor selection

Status: production fix and corpus validation in progress; automated regression deferred.

Observed trigger: four buffer descriptor words are Phi nodes at the merge of a direct
two-arm diamond. Each arm supplies host-evaluable SRT words, and the split condition is
also host-evaluable. The compact runtime resource plan does not retain the shader CFG,
so the selection must be lowered to typed `SelectU32` values before CFG disposal.

Required tests:

- A positive compiler-level case for both true/false edge orderings and four correlated
  descriptor words, checking that runtime materialization selects the expected descriptor.
- Rejection cases for divergent or undefined conditions, non-diamond control flow,
  loop-carried Phi nodes, mismatched Phi blocks, conditional raw reads, non-U32 values,
  and arms that are not valid runtime descriptor values.
- Neighboring checks showing invariant Phi handling and bounded descriptor tables remain
  unchanged, plus native Windows audits for every captured shader in this failure group.

## ReadLane-indexed descriptor tables behind scalar buffers

Status: production fix and corpus validation in progress; automated regression deferred.

Observed trigger: four correlated descriptor words are read through `ReadConstBuffer`.
Their record offset is selected with `ReadLane`; both the selected value and lane are
provably finite, but the runtime resource planner previously supported bounded tables
only when their backing source was a two-word raw address.

Required tests:

- A positive compiler case where a finite `ReadLane` key indexes four correlated
  `ReadConstBuffer` words and preserves the live GPU key in the specialized table.
- Materialization cases for zero and nonzero buffer stride, exact final-word access,
  base/offset overflow, negative immediate offsets, and coherent snapshot failures.
- Rejection cases for an unbounded source value, an out-of-wave lane selector,
  undefined lanes, malformed source descriptors, mixed table columns, and exceeded
  probe or resource limits.
- Equivalence checks against raw scalar-buffer reads and `ReadFirstLane`, followed by
  native Windows audits and a bounded Vulkan readback before upstream submission.

## Acyclic WorkgroupId coefficient reads stay on the GPU

Status: production fix validated in a bounded game run; automated regression deferred.

Observed trigger: a small acyclic compute shader reads one immutable coefficient per
`WorkgroupId` through a raw address. Eager CPU snapshotting fails when the source was
written by the GPU and is therefore unavailable to the coherent specialization reader,
although the existing BDA emitter can execute the same read directly on the GPU.

Required tests:

- An acyclic compute case where `WorkgroupId` indexes a raw scalar payload and resource
  tracking preserves `LoadAddressU32` instead of creating `ReadBoundedSrtU32`.
- GPU readback for the live BDA path across several workgroups, including wave64 split
  on a native subgroup-32 device.
- A neighboring cyclic/cooperative case proving that bounded immutable coefficient
  snapshots remain enabled where cross-wave scheduling requires them.
- Runtime rejection coverage for dirty specialization memory without converting an
  otherwise executable acyclic shader into a fatal materialization failure.

## Nested selection with an externally entered linear tail

Status: production fix and native shader validation in progress; automated regression
deferred.

Observed trigger: one arm of a nested conditional joins a straight-line tail that is also
entered by a sibling arm of the enclosing conditional. Global post-dominance assigns both
headers the same merge block. Giving the nested header an earlier merge allows a branch to
exit its selection illegally; retaining the shared merge sends the reducible shader through
the dispatcher fallback. The safe transformation clones the shared linear tail for the
nested path and gives that path a dedicated synthetic merge.

Required tests:

- A compiler case where either inner arm reaches a linear tail shared with a sibling arm of
  the enclosing selection, checking the cloned instruction range and dedicated inner merge.
- Nested and sequential variants proving that each `OpSelectionMerge` owns a distinct
  merge block and the result contains no dispatcher `OpSwitch`.
- Phi and register-state cases proving that mutually exclusive clones preserve edge values
  and execute the shared guest instructions exactly once on each original path.
- A positive case where the acyclic shared tail lies inside an enclosing loop, plus rejection
  cases for conditional or cyclic shared regions, multiple exits, and irreducible graphs;
  these require full semantic region cloning.
- Complexity-bound cases showing that large CFGs and graphs exceeding the small semantic-clone
  budget fall back promptly instead of repeatedly expanding the graph.
- Native Windows audit of captured shader `e52e19c6923301d0`, SPIR-V validation, and a
  bounded game run comparing compile time and generated word count with the dispatcher
  fallback baseline.

## Cooperative wave64 values live across scheduler phases

Status: implementation and native game validation complete; automated regression deferred.

Observed trigger: cooperative wave64 execution divides a guest CFG into software-scheduled
phases around LDS operations, subgroup collectives, guest barriers, and control-flow edges.
The previous backend assigned Function-storage slots to every typed runtime instruction, so
values used only inside one phase are repeatedly stored and loaded. In the captured
`916ea8893e5b276a` shader this produces roughly 11,561 `OpLoad` and 3,408 `OpStore`
instructions after optimization and makes a 120x68 dispatch take about 6.1 seconds.

The phase-liveness implementation reduced the optimized module from 170,999 to 146,134 words,
`OpLoad` from 11,561 to 8,993, and `OpStore` from 3,408 to 1,410. Vulkan and SPIR-V validation
passed in the native capture, but the measured dispatch remained about 6.05 seconds. The next
performance work therefore belongs to the cooperative scheduler/control-flow layer; the
regressions below still need to lock in this code-size improvement and its semantics.

Required tests:

- A compiler case with several ordinary arithmetic values used only within one cooperative
  phase, checking that they remain SSA values and receive no Function-storage spill slots.
- Cross-phase cases for guest barriers, LDS phases, subgroup collectives, CFG edges, branch
  conditions, and loop-carried Phi values, checking that every surviving value is spilled
  and reloaded before use.
- An immediate conditional-branch predicate, checking that spill analysis accepts constants
  without treating them as instruction-backed values.
- A mixed-use case where one definition has both same-phase and later-phase consumers,
  checking direct SSA use in its defining phase and a spill load in later phases.
- `ReadConstBuffer` broadcast and inactive-wave cases proving that stale private values cannot
  affect collective control flow or architectural results.
- SPIR-V validation plus GPU readback for multiwave LDS/SSBO ordering, early wave completion,
  differing loop counts, and same-address writes on a native subgroup-32 device.
- Native Windows capture of `916ea8893e5b276a`, comparing generated instruction counts and
  steady dispatch time while preserving the full output and guard readback.

## Depth comparison on non-depth color images

Status: upstream implementation selected; automated regression deferred.

Observed trigger: after reaching frame 93, a sampled color image with format 56 requested a
depth comparison. RDNA2 permits comparison sampling for this descriptor use, while Vulkan
`OpImageSampleDref*` requires a host format with native depth-comparison support. Treating the
color image as a Vulkan depth image fails specialization before the next frame.

Required tests:

- Resource-specialization cases for native depth formats and non-depth UNORM/sRGB/compressed
  formats, checking that only unsupported host formats select manual comparison.
- Single-sample, gather, offset, projected and bindless paths covering all eight comparison
  functions and the guest-defined `Dref op Dtexel` operand order.
- Sampler-splitting cases proving comparison state survives point/linear sampler cloning.
- Swizzle cases proving manual comparison reads the descriptor-selected source component.
- sRGB color formats checked through an equivalent UNORM view so depth-like values are not
  changed by hardware gamma decoding.
- Texture upload/view aspect tests for native depth images and ordinary color images.
- Cache-alias coverage where a single-sample `D32_SFLOAT_S8_UINT` depth target is later
  sampled through an equal-address, equal-footprint `R16G16_SFLOAT` color descriptor. The
  cache must replace the incompatible native image, preserve the raw 32-bit depth texels via
  the buffer-copy path, and create a color-aspect view without Vulkan validation errors.
- Neighboring depth-to-color alias cases for compatible byte widths, plus rejection or a
  diagnostic for unequal byte widths and multisample representations that cannot be copied.
- Native Vulkan readback comparing manual and hardware comparison where both are available,
  followed by a Yotei run beyond the frame-93 specialization failure.

## Acyclic buffer atomic returns in one split wave64

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: an acyclic compute shader with exactly one guest wave64 uses the old value
returned by a predicated `BufferAtomicIAdd32` as the element index for several output stores.
The native subgroup-32 backend keeps all 64 guest lanes in one host workgroup and emits the
buffer atomic once per active lane, but the execution planner rejected every live buffer
atomic result before SPIR-V emission.

Required tests:

- A planner case with one complete guest wave64 and an acyclic live buffer atomic result,
  checking that native subgroup-32 splitting retains a single 64-invocation host workgroup.
- SPIR-V and Vulkan readback cases where active lanes reserve unique output indices through
  `BufferAtomicIAdd32` and use the returned old values for scalar and vector buffer stores.
- Predicated and sparse-EXEC cases proving inactive lanes neither update the counter nor
  write output, including bounds-failure fallback values from the existing atomic emitter.
- Neighboring cases for the supported integer buffer atomic family, aliased counters,
  multiple atomics in one straight-line shader, and atomic results consumed by wave
  collectives without changing their lane ownership.
- Rejection cases for a live atomic result controlling a divergent branch, cyclic atomic
  feedback, cooperative scheduling, and partitioned multiwave workgroups until
  their separate ordering and publication contracts have executable coverage.
  Acyclic single-wave LDS and GDS integer atomic returns are covered by
  `--single-wave64-lds-atomic-return-only`,
  `--single-wave64-gds-atomic-admission-only`, and
  `--gds-atomic-add-return-wave64-only`.
- Native Windows audit and SPIR-V validation of the captured shader class, followed by a
  bounded game run beyond the original shader-admission failure.

## Bounded image descriptors in constant post-test loops

Status: production fix, native build, offline audit and bounded game validation complete;
automated regression deferred.

Observed trigger: a compute shader walks an eight-dword image descriptor table with a
loop-carried index initialized to zero. The descriptor is consumed before the latch increments
the index and repeats while the incremented signed value is less than the positive constant
bound. The bounded-read planner recognized only guards reached before the table read, and the
specialization path could turn bounded descriptor columns into buffer resources only.

Required tests:

- Planner cases for canonical post-test loops with `i = 0`, `next = i + 1`, and a backedge
  guarded by signed or unsigned `next < positive_constant`, checking the exact dense count.
- Rejection cases for zero, negative, overflowing, runtime, non-unit, decrementing, multiple
  latch and non-dominating update forms, plus a read that does not dominate the latch.
- An eight-column image table with consecutive dword offsets and one shared bounded selector,
  checking null/invalid descriptor normalization, candidate deduplication and dense key mapping.
- Mismatched count/address sources, strides, offsets, selectors, column widths and resource
  reuse must fail closed instead of combining unrelated descriptor words.
- SPIR-V and Vulkan readback cases for storage-image reads and writes through several table
  entries on native subgroup-32 wave64 execution, including repeated and all-null entries.
- Native Windows audit of the captured shader class and a bounded game run beyond its original
  `GetImageResource dword 0 is not a valid runtime value` failure.

## Scalar-buffer image tables with shifted wave-uniform selectors

Status: production fix and bounded game validation reached the next independent buffer boundary;
automated regression deferred.

Observed trigger: an eight-DWORD image descriptor was read from one scalar-buffer table with
the same `ReadFirstLane` selector multiplied by 32 using `ShiftLeftLogical32`. Resource
tracking recognized equivalent `selector * stride` and planned the existing inline image-table
materialization only for sampled images, so an `ImageRead` reached the generic runtime-value
validator with `ReadConstBuffer` roots.

Required tests:

- Four- and eight-DWORD scalar-buffer descriptors using `selector * stride` and
  `selector << shift`, proving equivalent keys produce the same inline table and GPU selector
  mapping.
- `ImageRead`, `ImageSampleRaw`, image-query and storage-image variants, including compact
  R128 descriptors, null/OOB table entries, repeated descriptors and the existing probe/dense
  image limits.
- Dominating selector limits, unknown wrapped-U32 selectors and scalar-buffer strides of zero,
  one, 16, 32 and non-power-of-two values, checking that valid in-range words are enumerated
  without inventing host descriptors.
- Writable/dirty table backing, unavailable specialization memory, alias overlap and
  transactionality cases, proving inline image materialization fails closed rather than
  snapshotting a GPU-written descriptor table.
- Exact `6cc64dee32dc7094` audit plus native game validation through guest PC `0x4d64`; the
  current run passes that image failure and stops next at a separate
  `GetBufferResource` scalar-buffer descriptor at PC `0x656c`.

## Guarded inline scalar-buffer descriptor tables

Status: synthetic RED/GREEN for the constant guarded-selector shape; exact Yōtei applicability
was disproved by a later captured variant.

Observed trigger: compute shader `6cc64dee32dc7094` reads four correlated descriptor DWORDs
with `S_BUFFER_LOAD_DWORDX4`, using one loop-carried `ReadFirstLane(Phi)` selector multiplied
by the 16-byte descriptor stride. A dominating unsigned guard bounds the selector, but resource
tracking previously supported this inline scalar-buffer pattern only for image and sampler
handles. The unchanged regression failed at guest PC `0x656c` with
`GetBufferResource dword 0 is not a valid runtime value (root=ReadConstBuffer)`.

The shared buffer path now reuses inline-descriptor recognition only when a dominating CFG guard
proves an exact nonzero selector limit. Coherent specialization reads all four descriptor words
for each candidate, produces one logical buffer table and leaves the live selector on the GPU.
Unguarded selectors and mismatched descriptor columns retain the prior closed failure. RED is
`_Build/logs/inline-buffer-table-red-20260908-v2.txt`; the unchanged selector and its two
negative boundaries are GREEN in `_Build/logs/inline-buffer-table-green-20260908-v2.txt`.

Validation correction:

- `_Build/runs/yotei-integrated-20260908-131508-5d7cde` reached frame 179 but did not compile the
  problematic `6cc64dee32dc7094` variant, so it cannot validate PC `0x656c`.
- `_Build/runs/yotei-integrated-20260908-132617-aaa93b` later reproduced PC `0x656c`; the exact
  variant uses a signed runtime Phi loop, stride 196 and an additional mask guard rather than the
  constant guarded-selector shape. This mechanism remains neighboring coverage, not the game fix.
- Add stride overflow/wrap, zero/OOB descriptors, unreadable/GPU-dirty backing, duplicate
  candidates, write-alias and 128-resource limit cases.
- Repair the pre-existing full `resource_tracking_tests` failure in the invariant indirect-image
  wrapped-immediate boundary; it occurs before the inline-table cases and is not counted GREEN.

## Dispatcher signed scalar-buffer descriptor loops

Status: synthetic RED/GREEN, exact captured-manifest audit and bounded native game retry pass;
the first nonzero frame remains pending.

Observed trigger: dispatcher compute shader `6cc64dee32dc7094` reads four correlated descriptor
DWORDs through an induction Phi starting at zero, unit increment, signed runtime count, an extra
bit-mask guard and 196-byte table rows. The existing `BoundedReadProof` already defined strict
signed-loop semantics, but the dispatcher path built only block indices and rejected the program
before canonical Phi proof.

The shared correction always builds the dispatcher graph and admits it to the same proof only
after the full CFG validates. RED `_Build/logs/dispatcher-signed-buffer-loop-red-20260908.txt`
failed at `GetBufferResource dword 0`; unchanged GREEN
`_Build/logs/dispatcher-signed-buffer-loop-green-20260908.txt` covers the positive two-row table,
zero and negative counts, bypassed count guard and non-unit increment. Exact audit
`_Build/logs/6cc64dee-dispatcher-signed-green-20260908.stdout.txt` completes resource tracking.

Bounded GPUAV run `_Build/runs/yotei-integrated-20260908-140320-cf909d` reaches frame 139, 124
shown frames and passes PC `0x656c`. The next fatal is `indirect image table at pc 0x000007c4 has
incompatible candidates`; it is a separate heterogeneous-image resource class and the next RED
target. No source readback was recorded in that quiet run.

The dimension-only heterogeneous-image subset is now covered by unchanged RED/GREEN selectors:
`_Build/logs/heterogeneous-indirect-images-red-20260908.txt`,
`_Build/logs/heterogeneous-indirect-images-green-20260908.txt` and
`_Build/logs/heterogeneous-indirect-images-spirv-green-20260908.txt`. Materialization accepts
sampled non-comparison candidates whose only differing layout property is 1D versus 2D and the
sample/read emitter derives coordinates per candidate. Numeric-class mismatch still rejects
transactionally; query/gather dimensions remain fail-closed.

Commit `22242aa` bounded run `_Build/runs/yotei-integrated-20260908-150356-e59dec` reaches frame
132 / 119 shown and passes PC `0x7c4`. The next fatal is PC `0x78b8`, where candidates differ in
both dimension (`3/1`) and shader swizzle (`0x24c/0`). The next RED must prove candidate-specific
swizzle semantics rather than weakening compatibility globally. Source frames 110–119 remain
RGB zero with alpha 3.

Remaining validation:

- Add signed-overflow/`INT32_MAX`, cyclic bound provenance, multiple latches and dirty-memory
  transactional boundaries without weakening the current fail-closed proof.
- Reproduce the exact PC `0x78b8` dimension-plus-swizzle candidates and extend only sample/read
  operations that can apply the swizzle per switch candidate; keep query/gather and incompatible
  numeric/conversion/comparison cases rejected.

## Signed integer storage images

Status: production fix, native build and bounded game validation complete; automated regression deferred.

Observed trigger: a compute shader writes one component through a regular eight-dword 2D image
descriptor whose guest format is `k16SInt`. Resource specialization rejected every signed
integer storage image even though the descriptor maps directly to a Vulkan signed integer
format and the shader does not use image atomics.

Required tests:

- Binding ABI coverage for signed storage images in each supported dimension, confirming that
  sampled signed, storage signed, storage unsigned and atomic unsigned arrays remain distinct.
- SPIR-V validation and Vulkan readback for `R8Sint`, `R16Sint`, `R8G8Sint`, `R16G16Sint`,
  `R8G8B8A8Sint`, `R16G16B16A16Sint`, `R32Sint`, and the wider signed 32-bit formats supported
  by the renderer.
- Store conversion cases for negative, zero and positive 32-bit shader values, including
  narrowing/clamping behavior of 8-bit and 16-bit signed image formats and component masks.
- Signed storage reads and read/write resources must return the original signed component bits
  to guest U32 registers; signed image atomics must remain rejected until their Vulkan contract
  is implemented explicitly.
- Mixed shaders using signed and unsigned storage images of the same dimension must allocate
  separate descriptor bindings and pass descriptor-budget checks for compute and pixel stages.
- Native Windows audit of `cs_0001dee4` and a bounded game run beyond shader
  `753c552fae650ec4` and its `storage image descriptor ... unsupported format 12` failure.

## GFX10 packed D16 formatted buffer loads

Status: production fix, native build, exact shader audit and bounded game validation complete;
automated regression deferred.

Observed trigger: compute shader `da7e70d9fcafe48c` repeatedly uses GFX10 MUBUF opcode
`0x83` (`BUFFER_LOAD_FORMAT_D16_XYZW`). The current decoder rejects the instruction before
CFG construction. GFX10 also defines the related `0x80` through `0x82` load forms, whose one
to four converted 16-bit components occupy one or two packed VGPRs.

Required tests:

- Decoder coverage for opcodes `0x80` through `0x83`, including exact destination DWORD and
  logical component counts, formatted metadata, addressing flags and raw diagnostic output.
- Vulkan readback for float, UNORM, SNORM, scaled, UINT and SINT descriptor formats, proving
  float-class results become IEEE half values while integer-class results retain their low
  sixteen result bits.
- Packing cases for X, XY, XYZ and XYZW, including preservation of the unused high half of
  the active destination VGPR for X and XYZ.
- EXEC-disabled and split-wave64 cases proving all destination bits remain unchanged for
  inactive guest lanes, including when VDATA overlaps VADDR.
- Descriptor swizzle and out-of-bounds cases covering memory, zero and one selectors and a
  format whose source record is wider than the packed destination.
- Native Windows audit of the captured shader class and a bounded game run beyond shader
  `da7e70d9fcafe48c` and its `unsupported family=MUBUF opcode=0x83` failure.

## Loop-indexed scalar-buffer descriptor tables

Status: production fix, native build, exact shader audit and bounded game validation complete;
automated regression deferred.

Observed trigger: a compute shader reads four correlated descriptor DWORDs from a scalar
buffer table. A loop-carried induction Phi starts at zero, advances by one, and contributes
`index * 196` to every table column. The loop is guarded by a signed comparison against a
runtime-uniform positive bound. Existing bounded-read proofs cover constant post-test bounds
and unsigned runtime pre-test bounds, so resource tracking leaves the descriptor words as
ordinary `ReadConstBuffer` values and later rejects them as host-unavailable runtime roots.

Required tests:

- Positive pre-test and post-test loops with `i = 0`, `next = i + 1`, a signed positive
  runtime bound and a 196-byte row stride, checking the exact materialized row count.
- Four correlated descriptor DWORD reads with consecutive memory offsets, checking that they
  share one coherent scalar-buffer snapshot and retain the live loop index for selection.
- Zero and negative signed bounds, signed overflow boundaries, `INT32_MAX`, unknown signs,
  cyclic bound provenance, non-unit updates, decrementing loops and multiple latches must be
  rejected or use explicitly defined zero-iteration/count semantics.
- Conditional or non-dominating scalar-buffer roots, mismatched column addresses, counts,
  strides, indices and offsets must fail closed instead of combining unrelated words.
- Materialization limits, address overflow, dirty-memory retry and aliasing cases must preserve
  transactional snapshots and never expose a partially read descriptor table.
- Native Windows audit and bounded game run beyond `da7e70d9fcafe48c` PC `0x000005d4`,
  followed by SPIR-V/Vulkan validation of the selected buffer resources.

## Misaligned tiled image descriptors

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: after 127 shaders compile and the loop-indexed descriptor shader creates a
valid Vulkan pipeline, image binding receives a 16x16 `k16_16_16_16Float` storage image using
`kStandard4KB`. Its SRD address is 256-byte aligned but has offset `0x800` within the 4-KB
allocation alignment returned by `TileGetTextureTotalSize`. That alignment describes a
standalone allocation; the descriptor's address describes a placed image view, and the detiler
already evaluates tiled offsets relative to that address while separately satisfying Vulkan
buffer-offset alignment.

Required tests:

- Diagnostic coverage proving every rejected image layout reports the guest address, computed
  size and alignment, format, type, tile mode, dimensions, levels and raw descriptor DWORDs.
- Linear and tiled image descriptors at exact, over-aligned and 256-byte placed-view addresses,
  including mipmapped, array, volume, multisample, sampled and storage resources. Allocation
  helpers must continue reporting the full tile-block alignment.
- Alias/view cases where a descriptor intentionally starts inside an existing backing image,
  checking whether a compatible subresource view is selected instead of creating a new image.
- Rejection cases for zero size/alignment, arithmetic overflow, unsupported tile modes and
  malformed descriptor address encodings. A placed view must retain its exact guest range;
  rounding the address down to the allocation boundary is forbidden.
- Vulkan validation and readback for any implemented rebase/copy path, followed by a bounded
  native game run beyond the current `descriptors.cpp` image-alignment failure.

Validation reached the original `053b2c82226fe5ed` storage write, created its Vulkan pipeline
and continued through four later compute pipelines without an image-layout fatal or Vulkan VUID.
The exact placed address was preserved rather than rounded to a tile boundary.

## Single-wave64 collectives on native subgroup32

Status: production split wave64 ballot is the 64-lane workgroup OR without
`OpGroupNonUniformBallot`; one complete guest wave stays on the split host
workgroup. Guest 32-bit LDS and collective scratch share one Workgroup `u32`
array (LDS prefix, scratch suffix) because SPIR-V Workgroup variables alias
without `WorkgroupMemoryExplicitLayoutKHR`. Sampled `OpImageFetch` no longer
takes ImageMemory control barriers; image writes still rendezvous. The compact
native-subgroup ballot and single-wave cooperative admission experiments were
reverted. Automated Vulkan/readback regression deferred.

Observed trigger: a complete 64-invocation guest wave uses subgroup operations on a
host whose native subgroup is 32. `OpGroupNonUniformBallot` mixed with workgroup
scratch aborts `nvgpucomp64.dll` on RTX 5060 Ti. Restoring the RTX 4060 64-lane
scratch OR lets the first 34 shader variants create pipelines and complete, including
the graphics pair `cc2b833287f423b9`/`2e17013fdae4d8ac`. The same compiler then
dies while compiling Screen Space Shadows `b90e2024732c6111` (`cs=0x8000399500`,
64×1×1, LDS, loops, ballot/image). RTX 4060 compiled that module (48 929 SPIR-V
words) and reached frame 125. Combining the Workgroup arrays (run
`yotei-integrated-20260907-163447-2da1c9`, SHA
`94eeb7449ba940752ea493c1b9efffbd256c38e7333fc7ae660a3220f03f8d1f`) and dropping
ImageMemory-after-fetch (`161953-de0599`) did not move that frontier.

Required tests:

- Native32 planner cases for 64-invocation wave64 collectives with and without
  LDS/barriers, proving one complete guest wave stays on the split 64-invocation host
  workgroup while multi-wave shared storage still selects the cooperative scheduler.
- SPIR-V validation proving split ballots use the 64-lane workgroup reconstruction,
  workgroup barriers and no `OpGroupNonUniformBallot`/`CapabilityGroupNonUniformBallot`.
  The reconstruction must not expand one ballot into a long per-invocation
  `OpLoad`/`OpBitwiseOr` chain: a bounded shared-word reduction may use
  `OpAtomicExchange`/`OpAtomicOr` (or an equally bounded workgroup primitive) and
  must preserve the two guest mask words.
- If the reduction uses the existing per-lane scratch slots, a leader-only
  aggregate write is a valid bounded alternative to per-invocation aggregate
  atomics. The regression must check that each logical wave publishes both
  aggregate words after the per-lane barrier, and must retain the same result
  for full, sparse and repeated predicates.
- Split+LDS modules declare exactly one Workgroup variable; guest LDS bounds stay
  the declared DWORD allocation; scratch AccessChains are `IAdd(lane, lds_dwords)`
  and must not be counted as competing LDS stores. Ballot aggregate words follow
  the complete per-lane scratch suffix with a separate two-word stride per
  cooperative wave; they must not alias `WavePublish` slots. 64-bit LDS atomics
  still emit a separate `u64` array.
- Vulkan readback for full and sparse/inactive EXEC lanes, repeated collectives,
  and looped LDS plus ballot, including a bounded analogue of the `b90e` loop/image
  pattern. Reusing the aggregate slots for a later ballot must not observe the
  previous mask, and cooperative multi-wave bases must remain disjoint.
- A shared `BitwiseOr32` used by multiple `IEqual32`/`INotEqual32` zero tests must
  lower each comparison through the equivalent De Morgan boolean form without
  emitting the integer OR; mixed nonzero consumers must retain the integer result.
- Host profiles with native subgroup64, native subgroup32 and unavailable subgroup-size
  control, proving the admission rule follows device capabilities rather than a title
  or shader hash.
- Native Windows game retry past `b90e2024732c6111` on RTX 50-series, comparing
  frame progress with the RTX 4060 result.

## Native32 wave64 barrier/image pipeline lowering

Status: b90e pipeline correction, native synthetic regression and the RTX 5060 Ti
game pipeline/dispatch path are GREEN; exact standalone probe compatibility and
nonzero surface output remain pending.

Observed trigger: the current `b90e2024732c6111` SPIR-V module passes
`vkCreateShaderModule` and `vkCreatePipelineLayout`, then crashes the NVIDIA compiler
inside `vkCreateComputePipelines` on the RTX 5060 Ti. The same lowering family must
remain valid when the guest wave64 contains LDS reads/writes, repeated barriers,
sampled image fetches, image publication and a 64-lane ballot. The test must not
identify the title, shader hash or guest address.

Required tests:

- A bounded synthetic one-wave64/native-subgroup32 module with LDS, a loop, ballot
  reconstruction, image fetch and image write, proving SPIR-V validation and the
  isolated `vkCreateComputePipelines` probe both complete without a driver fault.
- Lowering variants that remove each operation family independently, followed by a
  mechanically minimized public fixture, proving the failure is attributed to the
  shared wave64/barrier/LDS/image construction rather than module size or unrelated
  shader code.
- Split ballot output must use the defined 64-lane workgroup reconstruction, with
  exactly one aliased Workgroup `u32` allocation for LDS plus scratch where explicit
  Workgroup layout is unavailable; per-lane scratch starts after the LDS prefix and
  ballot aggregate words follow that scratch region without aliasing it.
  A 64-bit LDS atomic path must retain its separate typed storage.
- Collision-free DWORD stores whose byte address is exactly `(LaneId << 2)` or an
  EXEC-select of that address must use plain `OpStore` only for one complete
  64-invocation split host workgroup; arbitrary offsets, competing addresses,
  Function LDS, cooperative mode and non-64 host sizes must retain atomics.
- A split-ballot result consumed as
  `(lower & mask0) | (upper & mask1) != 0` must preserve the independent mask
  semantics while lowering the final zero-test without an integer OR over
  Workgroup-derived values. The regression must check the value-graph semantics
  (for example, De Morgan `!(lower_mask == 0 && upper_mask == 0)`), not only
  opcode counts. This is a narrow zero-test optimization; arbitrary guest
  integer OR operations must retain their exact 32-bit result.
- Sampled image fetches must not acquire `ImageMemory` publication semantics;
  image writes and image atomics must retain the required rendezvous and their
  publication scope must be `Workgroup`, not a device-wide scope invented by the
  lowering.
- Neighboring native subgroup64, wave32, no-LDS, read-only-image, inactive-EXEC and
  multi-wave/cooperative cases must keep their existing admission and SPIR-V
  contracts. Unsupported cyclic or cross-wave cases must still reject closed.
- Run the exact saved module through `vk_spv_probe.exe` before and after each
  candidate, record the exit/output, and only then retry the real 1280x720 game
  path with all owned processes confirmed stopped.

The current isolated minimization of fresh module
`_Build/runs/yotei-integrated-20260907-183157-411634/...b90e2024732c6111.spv`
produced a valid neutral-body module that passes both `spirv-val` and the isolated
probe. Independent family variants showed that removing barriers, atomics or image
operations alone does not clear the crash, while neutralizing loads, access chains or
all integer ORs does. Delta probing then isolated one downstream OR (ordinal 23,
`(lower & mask0) | (upper & mask1) != 0`) as the necessary compiler-sensitive
shape in that module. The shared emitter now rewrites only this semantically narrow
zero-test form to logical comparisons; arbitrary integer OR results remain unchanged.
The synthetic RED/GREEN artifact and probe results are retained under `_Build/analysis`.
The fresh game run `_Build/runs/yotei-integrated-20260907-211101-9da340` reached
frame 158 with `b90e2024732c6111` and `a7661ff4ea282325` completed. The exact
captured `b90e` artifact still crashes the minimal standalone probe during
pipeline creation, while the real game layout creates its pipeline successfully;
the difference is retained as an open probe-layout boundary. Eight source
readbacks remain RGB zero with alpha 3, so this result does not claim the first
nonzero frame.

## DWORD pattern fills and sampled HTILE clear materialization

Status: shared production fix and native synthetic GPU regression are GREEN; a
current-tree game readback remains pending behind the separate unfinished wave-ballot path.

Observed trigger: the compute-clear fast path recognized only the 16-byte `uint4`
fill shape. A full 4-byte DWORD pattern dispatch targeting registered HTILE was
therefore executed as an ordinary buffer shader without carrying its exact clear
encoding into depth metadata. The initial unchanged regression failed with
`complete dword-pattern HTile fill was not recognized`. Extending the fixture to
sample the resulting native D32 exposed a second failure: consuming only the logical
metadata state left stale raw guest metadata, and a repeated texture acquisition
materialized the old clear.

The shared correction now requires the exact descriptor, ten-user-SGPR pattern,
64x1x1 wave geometry, full group coverage, repeated DWORD value, write-only resource,
and absence of snapshots/images/samplers/DMA before replacing a dispatch. Rejected
partial, non-uniform and snapshot-dependent variants preserve the caller outputs.
An accepted metadata fill records its exact value and performs the same guest-memory
buffer fill; registered HTILE then maps only canonical `0` and `0xfffffff0` encodings
to native depth 0/1. Depth/stencil load clears use their respective HTILE bit fields,
and unsupported encodings still reject closed. No title, shader hash or guest address
is part of the mechanism.

Validation at `b1a894f-dirty`:

- RED log: `_Build/logs/pattern-htile-20260908/red.stderr.log`.
- GREEN log: `_Build/logs/pattern-htile-20260908/final-green.stdout.log`; executable
  SHA-256 `fc092c5ab176820803b25fa59b332e5f07879aa6c6ad4052c790f893e3198f97`.
- `--sampled-htile-clear-only`, `--sampled-htile-array-clear-only`,
  `--compute-meta-clear-only`, `--sampled-htile-admission-only`, and
  `--native-htile-subset-only` all pass. The clear selector also passes with the
  portable `VK_LAYER_KHRONOS_validation` loaded and GPU-assisted validation enabled.
- The modified current-tree production/test translation units compile with clang-cl.
  A normal full target remains blocked because neighboring dirty
  `spirvEmitterProgram.cpp` references missing
  `EmitterState::wave_ballot_word_variables`.
- Two bounded test-only game runs with executable SHA-256
  `57d425db107c4347fff58e1313da06b470d5879a9cab088ab9a41bc611bcaaeb`
  (`yotei-pattern-htile-current-20260908-092819-bb437d` and warm-cache
  `yotei-pattern-htile-current-warm-20260908-092936-fda3a1`) stopped at the prior
  `b90e2024732c6111` driver breakpoint before guest VideoOut/readback. The executable
  used only a temporary zero-initialized declaration to complete the unrelated dirty
  build; that declaration was removed immediately afterward. These runs do not prove
  the first frame or regress the HTILE synthetic result.

Remaining validation: restore a normal current-tree build by completing the ballot
change, repeat the bounded game run, and require at least one source readback with
nonzero RGB. Menu and gameplay remain separate pending stages.

## Partitioned graphics loop budget at the structured back-edge

Update 2026-10-03: a temporary Vertex budget of four iterations reached the
119856-index draw but produced invalid SPIR-V before GPU execution. Validator:
`OpPhi's number of incoming blocks (2) does not match block's predecessor
count (3)` in `_Build/runs/yotei-integrated-20261003-120114-presentfix-gpuav-sync`.
The budgeted back-edge adds a path to the loop merge without updating a merge
Phi. The new public `shader_cfg_tests
--partitioned-graphics-loop-merge-phi-only` produced the same validator RED
before the shared fix (`_Build/logs/loop-merge-phi-red2-20261003.txt.stderr`).
The emitter now applies the budget at an existing conditional exit when that
exit is the sole predecessor of the continue block. The unchanged merge Phi
retains the same predecessor set. If the merge has Phi values but no such
existing exit, the emitter does not add an invalid direct edge. The same test
is GREEN (`_Build/logs/partitioned-graphics-loop-merge-phi-green-20261003.txt`),
including a counter-update check; original partitioned loop, graphics wave64
collective and nested conditional-latch neighbors are GREEN. A temporary
Vertex cap still needs a bounded game retry as diagnosis, and is not a
semantic replacement for wave64.

Status: the synthetic validator regression and the captured game SPIR-V are GREEN;
the full native suite and nested-loop variants remain deferred.

The previous finite pixel-loop guard emitted a kill selection at the first
non-Phi instruction of the loop body. When that body was also the SPIR-V continue
target, the selection was not structurally post-dominated by the back-edge block.
A direct budget exit from the loop header avoided that error but introduced a new
path to the merge that bypassed body definitions. The unchanged captured shader
then failed dominance validation.

The shared emitter now records the structured loop header, merge and continue
blocks and applies the finite budget on the continue back-edge. The over-budget
edge reaches the existing merge only after the body has executed, preserving
dominance, while `OpLoopMerge` remains immediately adjacent to its branch.
`shader_cfg_tests --partitioned-graphics-loop-only` covers conditional while and
do-while back-edges; its original RED is retained at
`_Build/logs/partitioned-graphics-loop-red-20260908-v3.txt` and GREEN at
`_Build/logs/partitioned-graphics-loop-green-20260908-v2.txt`. The resulting game
module `f8927c09f4b928c7` also passes standalone `spirv-val`.

Remaining validation: run the complete native test suite and add explicit nested
and multiple-loop cases, including distinct continue targets and merge blocks.

## GFX10 VOPC V_CMPX_NE_U16

Status: production decoder/lowering, unchanged synthetic RED/GREEN, full CPU
corpus audit and runtime reproduction complete; bounded post-fix game retry pending.

Observed trigger: native run
`_Build/runs/yotei-integrated-20260908-171938-6e189d` on `672af1f` completed the
previous `6cc64dee32dc7094` and `34e090c623ad611c` frontiers, reached frame 222,
then exited itself with code 321 while building CFG for compute shader
`c8e8f554efbfadef`. The exact instruction at PC `0xf4` is compact VOPC raw
`0x7d7a40ff`, opcode `0xbd`. LLVM's GFX10 VOPC table identifies `0xbd` as
`V_CMPX_NE_U16`: compare the low unsigned halfwords for inequality, write the
lane mask and update EXEC. No title, shader hash, address or install-path branch
is involved.

The final regression uses the captured first DWORD with a controlled literal and
checks two-word decode, operands, EXEC destination, unsigned 16-bit inequality IR,
dynamic EXEC update, decoded text and SPIR-V validation. The unchanged test first
failed with `decoder rejected captured VOPC V_CMPX_NE_U16 fields`; after `5aaf3b4`
it prints `KYTY_VOPC_CMPX_NE_U16_PASS`.

Full corpus audit
`_Build/shader-audits/yotei-current-20260908-vopc-bd/report.json` passes 741 of
825 manifests. Both previous `0xbd` groups are absent: 19 compact manifests and
7 SDWA manifests now pass their captured audit depth. Remaining validation:

- Build and hash the full native `kyty_emulator` at the committed revision.
- Run the same bounded GPUAV-lite/source-readback profile using the preserved
  18 031 187-byte pipeline cache, prove `c8e8f554efbfadef` passes, and record the
  next first fatal or first nonzero RGB.
- Keep the 84 remaining corpus failures as independent debt. Do not claim them
  fixed from this opcode result, and do not prioritize them over an earlier
  runtime frontier without separate evidence.

## Local-region shared-tail CFG cloning and Yotei compile latency

Status: synthetic neighbors, exact manifests, full CPU corpus and bounded native
game timing are GREEN; first nonzero surface output remains pending.

The captured `6cc64dee32dc7094` graph has 346 blocks and one safe shared linear
tail in a two-block candidate region. The previous whole-graph cutoff rejected
that local rewrite, forced dispatcher fallback and produced a 359,238-word
SPIR-V module whose NVIDIA driver compile stalled for more than 15 minutes.
Removing the whole-graph cutoff without a local bound made neighboring
`ps_00051f2c.json` pathological because its candidate region contains 117/118
blocks.

Checkpoint `f1776f0` admits semantic cloning only while the local region has at
most 32 blocks and before synthetic `GotoVariable` routing exists. Existing
single-shared-block, straight-line, 16-instruction-tail and four-clone limits
remain. `shader_cfg_tests --shared-merge-cfg-only` covers seven neighboring CFG
shapes. Exact `6cc64dee…` is structured with 376 blocks and its full audit drops
from about 13.02 to 3.45 seconds. `ps_00051f2c.json` remains bounded through its
dispatcher path. Full corpus result is 743/825 in 37.851 seconds versus 741/825
in 57.019 seconds, with no new status regression.

Native GPUAV-lite run
`_Build/runs/yotei-integrated-20260908-200400-74722b` naturally reaches frame
133 in 145 seconds versus 1123 seconds for the prior comparable diagnostic run.
Warm run `_Build/runs/yotei-integrated-20260908-201027-d8c982` naturally reaches
frame 130 in 120 seconds. Its 118 source readbacks are still RGB zero/alpha 3.

Remaining validation:

- Keep pathological large-region and post-`GotoVariable` cases bounded while
  adding future structurization rewrites.
- Measure steady rendering FPS only after a nonzero frame exists; the current
  figures are startup/compiler progress, not gameplay performance.

## GPUAV shader instrumentation cache identity

Status: **shared cache-identity fix verified in native synthetic test**. Before
the fix, `DriverCacheFileName` and `FormatDriverCacheSignature` took only the
GPUAV enabled bit. A GPUAV run with `VK_LAYER_GPUAV_SHADER_INSTRUMENTATION=0`
and one with `=1` therefore selected the same `-gpuav.bin` blob and `v1`
signature, although the validation layer can hand different SPIR-V modules to
the driver.
The 3 October no-instrumentation run loaded the 306192646-byte GPUAV cache and
then hit `0x80000003` while creating compute pipeline `b90e2024732c6111`.
This was a collision in the cache identity, but the crash cause remains
unproved: an isolated no-layer probe also crashes on that module. The same
synthetic `shader_cfg_tests --pipeline-cache-instrumentation-only` failed before
the fix with `distinct validation instrumentation modes shared a driver cache
file` (`_Build/logs/cache-instrumentation-red-20261003.txt.stderr`) and passed
after the fix (`_Build/logs/pipeline-cache-instrumentation-green-20261003.txt`).
The native neighboring validation-mode, revision and build-identity checks also
passed. The three modes now use `-core.bin`/`v0`, `-gpuav-lite.bin`/`v2`, and
`-gpuav-instr.bin`/`v3`; the old mixed `v1` signature is rejected. Next: native
emulator build and a bounded game retry with the new isolated cache. Do not
interpret the identity test as proof that it fixes the `b90e` compiler crash.

## GPUAV-sensitive NVIDIA pipeline cache identity

Status: **shared identity fixed** (KytyPC3 + separate `{TITLE}-core.bin` /
`{TITLE}-gpuav.bin`). No-GPUAV game retry after cool-down still required to
confirm early graphics pipeline with a clean core cache.

Two bounded runs using a cache warmed under GPUAV-lite but launching without
GPUAV fail at frame 12/13 inside NVIDIA `nvgpucomp64.dll` 32.0.16.1664 with
exception `0x80000003`. Windows Event Log attributes both APPCRASH events to the
same module offset. The instrumented GPUAV-lite path continues to the later
emulator frontiers. Driver cache blobs from GPUAV-lite and non-GPUAV attempts are
therefore preserved under separate filenames and must not be mixed.

Tests:

- GREEN: `shader_cfg_tests --pipeline-cache-validation-mode-only` →
  `KYTY_PIPELINE_CACHE_VALIDATION_MODE_PASS` (distinct filenames; core vs GPUAV
  signatures incompatible; self-compatible). Neighbors
  `--pipeline-cache-revision-only` / `--pipeline-cache-identity-only`.
- Keep the native driver reproduction bounded and do not repeatedly crash the
  compiler merely to test cache loading.
- Next: after a long DeviceLost cool-down (GPUAV+instr recovers; core/noval and
  GPUAV-lite still abort in `vkCreateComputePipelines` for CS `0xb90e…` with
  `exit=-2147483645` for ≥12m), run no-GPUAV `ContinueAfterColored` with empty
  `_PipelineCache/PPSA26344-core.bin`. Do not load the legacy mixed
  `PPSA26344.bin`. Separately, consider tagging shader-instrumentation in the
  cache identity (lite vs instr currently share `-gpuav.bin`).

## GFX10 MUBUF opcode 0x87 runtime frontier

Status: exact native runtime RED captured; decoder/lowering regression and fix
pending.

Run `_Build/runs/yotei-integrated-20260908-200400-74722b` naturally exits while
building compute shader `c8e8f554efbfadef` at PC `0xc6c`. The decoded family is
MUBUF opcode `0x87`, raw words `[0xe21c6000 0x8002000e]`; CFG construction rejects
it as unimplemented. This is earlier and more actionable than speculative FPS
ports because execution cannot pass the instruction.

Required tests:

- Identify opcode `0x87` from the GFX10 ISA tables and add an exact raw-word RED
  covering operands, format/packing and destination footprint.
- Implement the shared decoder/IR/backend semantics without title, hash, address
  or install-path branches; keep unsupported neighboring formats fail-closed.
- Run unchanged GREEN, neighboring opcode cases, the 825-manifest corpus and a
  warm GPUAV-lite/source-readback game retry.

## First nonzero Yotei source frame

Status: **rendered-pixel milestone reached on current tip; menu and gameplay remain pending.**

Historical proof on `96611fe`: GPUAV-lite run
`_Build/runs/yotei-integrated-20260909-101008-669804` reached `shown=280`.
Source readback stayed black through frame 235; frame 236 had 10 nonzero RGB
pixels and frame 242 had 214. Screenshot
`_Build/analysis/yotei-first-nonzero-96611fe.png` shows the white loading
spinner.

**Re-proven 26 September 2026** on bring-up tip `fb1cc8b` / plan note `23e4fd74`:
warm GPUAV+instrumentation fullscreen run
`_Build/runs/yotei-integrated-20260926-081515-presentfix-gpuav` —
prepared **frame 250** `colored=10` (max RGB 45/45/45), then frames 251–276
grow to `colored=108` / max ~546/525/546; `shown=277`, auto-stop
`colored-proven` (~147 s). Present blit after Flip EOP is the readback site.

Remaining validation:

- Preserve the readback as the evidence boundary: do not promote the result to
  menu or gameplay until a recognizable full scene / menu frame is captured.
- Continue bounded runs past spinner (`ContinueAfterColored`); record the first
  post-spinner compile, resource, GPU execution, or presentation blocker.
  Evidence 26.09 `…-081840-presentfix-gpuav`: frame watchdog at **shown=159**
  while CS `54904fb419d79e49` did **4×** specialization recompile —
  `vkCreateComputePipelines` ≈289.8 / 295.5 / 286.8 s, fourth `begin` without
  `done` at Kill. ReadbackStart=240 never reached (`sawColored=false`).
- After `/STACK:16777216` + SPIR-V permutation reuse (`…-093517`): reached
  **shown=197**, then Fatal `MaterializeResources` (exit 321). Menu still pending.
- Dense buffer limit: raise shared `ShaderInfo::MaxBuffers` 64→128→512 (device
  DescriptorBudget remains the hard gate). `…-112601` cleared densify then hit
  RO SSBO host `adjustment=2` (`storage buffer offset adjustment is unsupported`).
- Shared RO realign: `NativeStorageBuffer` stages Upload when dword-indexed SSBO
  has byte host adj and `read && !written && !atomic` (non-GPU-dirty). RED/GREEN:
  `shader_recompiler_compute_tests --storage-buffer-ro-aligned-upload-only`
  (`_Build/logs/ro-align-{red,green}.out.txt`). Writable/atomic/GPU-dirty stay
  fail-closed. Long run after this tip: `…-120514-presentfix-gpuav` in progress.
- Debt for shared fix: print/use `LastResourceSpecializationError` on that Fatal;
  continue specialization-stable pipeline identity / smaller SPIR-V under GPUAV —
  not a title branch and not “merge #718 first”.
- Re-run the accumulated shader/GPU test debt before upstream submission; the
  rendered-pixel milestone does not waive neighboring regressions.
