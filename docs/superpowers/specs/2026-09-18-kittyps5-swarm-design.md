# KittyPS5 Swarm Design — 2026-09-18

Goal: broader boot reach (2D + UE4/5/Unity) + faster boot/VM alloc, no ctest regressions.
Upstream: KytyPS5/KytyPS5 @ 104530e, branch `swarm/boot-reach`.

## Teams (all ponytail-full)
1. GPU/Shader — `src/graphics/`, tests: ShaderRecompilerCompute, ShaderVertexMetadata, shaderCfg, ResourceTracking, ResourceMaterialization, ImagePageTable
2. Kernel/HLE — `src/kernel/`, `src/libs/`, tests: KernelFileSystem, EventQueueLifetime, SyncOnAddress, ImeDialog, AudioOut2
3. Loader/VM — `src/loader/`, pageManager, MemoryTracker, VirtualMemoryAllocation, BitArray
4. Perf/Harness — `tests/` runner, launcher, boot timing, CI
5. Reviewer — spec compliance + ponytail quality gate, final branch review

## Ponytail contract (every agent)
Ladder: reuse codebase → stdlib → native → installed dep → one-liner → min diff.
Root-cause fix in shared fn, not per-caller. One runnable check for non-trivial logic.
Mark ceilings with `ponytail:` comment. Never simplify away validation/error handling/security.
Skipped / when-to-add in ≤3 lines.

## Boundaries
- One domain per agent; `src/common/` read-only unless reviewer approves.
- Windows behavior unchanged unless explicitly scoped (Linux/macOS guards only).
- Controller holds ledger, dispatches sequentially for writes, parallel for scouts.

## Flow (subagent-driven-development)
Scout (parallel) → plan → implement per-task + task review (≤5 fix rounds) → final reviewer → finishing branch.
Reviewer verdicts: spec ✅/❌ + Critical/Important/Minor. Minors deferred to ledger, parked only at cap with ruling.

## Done
- More titles boot past loader to render; no new fatal in PageManager/VM path.
- `ctest` 17/17 pass (or baseline-equal), boot/VM alloc measurably faster.
- Shortest diffs; no new deps without reviewer sign-off.
