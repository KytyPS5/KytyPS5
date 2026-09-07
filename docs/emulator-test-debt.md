# Emulator regression test debt

This file records regression coverage deferred during fast launch bring-up. Each item
describes a guest contract rather than a title-specific workaround. Deferred tests must
be added before the corresponding fixes are proposed upstream.

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
