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
