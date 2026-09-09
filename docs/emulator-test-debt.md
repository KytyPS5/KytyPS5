# Emulator regression test debt

This file records regression coverage deferred during fast launch bring-up. Each item
describes a guest contract rather than a title-specific workaround. Deferred tests must
be added before the corresponding fixes are proposed upstream.

## Full-suite failures retained after upstream sync

Status: Windows CI-equivalent tests pass; extended local CTest is 46/51 after
merge `a309653` (`upstream/main` at `0b4e78c`).

The remaining failures are outside the three-test Windows CI gate and were not
hidden or disabled during conflict resolution:

- `shader_cfg`: exact `wide-buffer` SPIR-V budget is stale (actual 831 words / 216
  instructions versus 807 / 211).
- `scalar_provenance`: out-of-bounds constant-buffer walk is not transactional.
- `resource_tracking`: invariant indirect-image proof admits a wrapped scalar
  immediate; this is the pre-existing debt already tracked below.
- `shader_recompiler_compute` and `texture_cache_image_overlap`: ordinary sampled
  aliases do not consistently retain/rebind the live comparison-depth owner.

Resolve these as separate regression-first mechanisms. They are not evidence
against the newly merged DS bounded atomics, DB render override, APR, CES or
buffer-cache changes, whose focused tests pass.

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
  longer exceeds 60 seconds before claiming menu or gameplay progress.

## Compressed video-out metadata on a native render-target alias

Status: regression test added; run against the unfixed implementation before the
shared cache correction.

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
  feedback, LDS atomics, cooperative scheduling, and partitioned multiwave workgroups until
  their separate ordering and publication contracts have executable coverage.
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

## GPUAV-sensitive NVIDIA pipeline cache identity

Status: runtime workaround documented; shared cache identity regression and fix
remain pending.

Two bounded runs using a cache warmed under GPUAV-lite but launching without
GPUAV fail at frame 12/13 inside NVIDIA `nvgpucomp64.dll` 32.0.16.1664 with
exception `0x80000003`. Windows Event Log attributes both APPCRASH events to the
same module offset. The instrumented GPUAV-lite path continues to the later
emulator frontiers. Driver cache blobs from GPUAV-lite and non-GPUAV attempts are
therefore preserved under separate filenames and must not be mixed.

Required tests:

- Add a CPU cache-signature regression proving shader instrumentation/validation
  mode participates in compatibility whenever the layer can change modules seen
  by the driver.
- Keep the native driver reproduction bounded and do not repeatedly crash the
  compiler merely to test cache loading.
- Re-enable a no-GPUAV fast profile only after the cache identity is separated
  and a clean-cache run passes the early graphics pipeline.

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

Status: **rendered-pixel milestone reached; menu and gameplay remain pending.**

Native GPUAV-lite run `_Build/runs/yotei-integrated-20260909-101008-669804` at
checkpoint `96611fe` reached `shown=280`. The unchanged source readback is black
through frame 235, then frame 236 contains 10 nonzero RGB pixels and frame 242
contains 214. `_Build/analysis/yotei-first-nonzero-96611fe.png` independently
shows the white animated loading spinner in the upper-right corner.

Remaining validation:

- Preserve the readback and screenshot as the evidence boundary: do not promote
  the result to menu or gameplay until a recognizable full scene is captured.
- Continue bounded runs with the 60-second shown-frame watchdog and record the
  first post-spinner compile, resource, GPU execution, or presentation blocker.
- Re-run the accumulated shader/GPU test debt before upstream submission; the
  rendered-pixel milestone does not waive neighboring regressions.
