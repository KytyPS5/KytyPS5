# Emulator regression test debt

This file records regression coverage deferred during fast launch bring-up. Each item
describes a guest contract rather than a title-specific workaround. Deferred tests must
be added before the corresponding fixes are proposed upstream.

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

Status: production fix, native build, and exact captured-shader audit complete; runtime
materialization/game validation and automated regression deferred.

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
Runtime descriptor materialization and SPIR-V/Vulkan execution still require the bounded game run.

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
