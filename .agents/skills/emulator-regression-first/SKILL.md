---
name: emulator-regression-first
description: Fix emulator defects and implement missing guest behavior by proving a regression before the fix and correcting shared mechanisms across games. Use for KytyPS5 shader, GPU, resource, kernel, ABI and scheduling changes; not for documentation-only tasks.
---

# Regression-first emulator fixes

Treat a failing game as a reproduction lead. Establish whether the cause is an emulation
defect, invalid input or an unsupported configuration. For an emulation defect, reproduce
the guest contract independently and correct it for all supported inputs.

## Establish the contract and a failing test

- Inspect the current code, repository instructions, revision, worktree changes and
  failing artifact. Separate decoding, control flow, resource planning/materialization,
  SPIR-V validation, GPU execution and presentation failures.
- Verify uncertain semantics against primary ISA/API/ABI documentation. Do not derive
  expected behavior from the current implementation or from output that merely advances
  one game. Keep unproven behavior explicitly unsupported.
- Before production fixes, add a minimal synthetic regression to the existing relevant
  harness. Preserve the semantic trigger without copying proprietary shader/game bytes.
  Compute expected results independently; test observable behavior or a meaningful
  intermediate invariant, not just the existence of the new code.
- Run the test against the unfixed production revision. Save its command, revision,
  failure and artifact path. Confirm the failure is the intended defect. If it already
  passes, improve the reproducer or revise the diagnosis before implementing a fix.
- Bound shader loops, allocations and worker lifetimes. A deterministic wrong-result
  check or compiler-level invariant is preferable to deliberately hanging the GPU.
  Do not weaken validation or alter the host timeout policy to obtain a reproduction.
- If a production patch already exists, preserve it and prove the test fails with only
  that patch absent, using an isolated checkout or a scoped patch reversal. Never undo
  unrelated user work. Until this evidence exists, keep the patch unvalidated.

## Implement the shared correction

- Change the layer that owns the semantics; reuse existing instruction tables, typed
  IR operations, descriptor models and capability checks. Generalize within the actual
  contract without introducing a second path for each game or speculative frameworks.
- Never choose behavior by game title/ID, shader hash, recorded address, path or a
  lucky constant from one capture. Those identifiers may label diagnostic artifacts.
  Legitimate ISA generations and host capability differences need explicit inputs.
- Preserve distinctions such as per-lane predicates versus wave-wide masks, guest
  addresses versus host backing, ordinary reads versus synchronized resource snapshots,
  and software work budgets versus device limits.
- Do not replace missing semantics with success stubs, zero-filled resources, dropped
  commands or ignored validation errors. A documented zero-work operation may be skipped
  according to its contract. Reject unsupported configurations with useful diagnostics.
- In parallel work, assign file ownership and let diagnosis/test design run independently.
  Production implementation starts only after the reproducer has failed as intended.

## Prove correctness beyond the captured case

- Run the same regression after the fix. Preserve its oracle; change an expectation only
  when primary semantics prove the old expectation wrong, and explain that correction.
- Add focused variations for the affected contract: for example, sparse and full masks,
  register overlap, first/last valid elements, out-of-range accesses, resource counts,
  descriptor origins, formats or supported wave sizes. Choose relevant cases rather
  than multiplying every dimension indiscriminately.
- Check compiler/IR behavior and real GPU readback where the change reaches GPU execution.
  Validate native Windows artifacts when Windows is the target. Serialize builds and
  GPU-heavy runs; include the shader and Vulkan validation paths where applicable.
- Run affected existing tests and required repository checks. Audit all available relevant
  corpora, including other games when present. Missing games or hardware are coverage
  limits, not permission to label an approximation correct. Do not lower a wave64 test
  to wave32 and report full wave64 coverage.
- Retry the original workload within the user's existing authorization. A passing parser,
  resource audit, build or shader test is not proof of a rendered frame/menu/gameplay.

## Record the result

Report the semantic defect and shared correction, failing-before/passing-after evidence,
neighboring cases checked, native build/runtime result and remaining coverage limits.
Keep local captures and run logs as evidence; commit reusable tests and source changes.
Do not claim completion while the requested runtime outcome is still failing.
