---
name: ps5-emu-optimization
description: Playbook for making a PS5 emulator (Kyty, shadPS4-style HLE + Vulkan) run a real game faster and without stutter, and for reverse-engineering the game's code and shaders. Use when asked to raise fps, remove hitches/stalls, find a bottleneck, judge whether an optimization idea is worth trying, design an A/B benchmark, debug a crash in guest or host code, disassemble guest code, extract/precompile shaders, or patch a PS5 game binary.
---

# PS5 emulator optimization and game reverse-engineering

Distilled from ~100 perf commits, ~150 recorded experiments and the tooling built while taking
Demon's Souls (PPSA01341) on KytyPS5 from ~21 fps to ~40 fps, and from multi-second shader stalls
to none. Numbers are from that project (i9-14900K + RTX 5090); treat them as orders of magnitude.

Source: the `experiment/perf-40fps-20260926` branch of https://github.com/chenxiao07/KytyPS5 (an
earlier snapshot: https://github.com/KytyPS5/KytyPS5/pull/599). The `KYTY_*` switches, scripts and
file names mentioned here live in that branch; take them as examples and adapt the ideas to your tree.

Read the reference that matches the task:
- `references/lessons.md`: what worked, what failed and why (check before proposing an idea).
- `references/measurement.md`: benchmark harness, A/B protocol, noise, correctness gates.
- `references/reverse-engineering.md`: guest address model, disassembly, shaders, game patches, crash triage.

## Core model: where the time goes

An HLE PS5 emulator runs the game's x86-64 CPU code natively and translates its GPU command buffers
(PM4) and shaders (RDNA2 -> SPIR-V) at run time. In practice:

1. **One thread translating the graphics queue is the bottleneck.** The game's own CPU work ran
   headless at ~100 fps; the render thread was 84% busy at 40 fps. The GPU is idle for much of the
   frame waiting for translation. Only time on that thread converts ~1:1 into frame time.
2. **Per-dispatch/per-draw fixed cost dominates**, not per-byte cost: ~4000 dispatches and ~1000 draws
   per frame at 1-5 us each. Resource-table (SRT/V#/T#) evaluation, buffer lookups, dirty checks,
   descriptor binding and barriers are the hot categories.
3. **Guest-visible memory coherence is the second tax**: write-protect/fault tracking of guest pages
   (hundreds of mprotect/VirtualProtect calls per frame), CPU->GPU uploads, GPU->CPU readbacks the
   game waits for, and invalidation after unmap.
4. **Stutter is a separate problem from fps**: first-use shader translation + driver pipeline
   compile (100 ms - several s), cache/pool re-creation after the game unmaps memory, and
   synchronous waits hidden in rare paths.

Always establish which of the four you are looking at before changing code.

## Method (follow in order)

1. **Fix the scene and the metric.** A reproducible spot (fixed save, scripted input) plus a
   moving route; report fps as total frames / total time and the worst 1-s window. Screenshot every
   run; an fps-only benchmark once hid black triangles for days.
2. **Profile the render thread with symbols of the exact build** (linker map, not a stale PDB), and
   split it by work kind per frame (dispatch prep, draws, waits, sync downloads, faults).
3. **Prove causality before optimizing.** Inject a known delay (e.g. +1.2 ms/frame) at the candidate
   site and measure the frame-time slope. Slope ~1 = on the critical path; ~0 = saving it gains
   nothing (work on async queues or inside GPU-bound windows). This single probe explained why many
   "saved 0.8 ms" changes gained 0 fps.
4. **Measure the upper bound first.** Make the unsafe version (skip the work entirely) and measure.
   If the ceiling is small, stop. Then subtract the real cost of the safe replacement
   (bookkeeping, proofs, fallbacks). Most failed ideas had a real ceiling eaten by bookkeeping.
5. **Implement behind a default-off switch** (env var or a 32-bit global you can poke live), with a
   verify mode when the change replaces correctness work (compare old vs new result, count mismatches).
6. **A/B in the same process**, alternating A/B/A/B at the same spot (live toggle), several rounds.
   Cross-session numbers drift +-1-3 fps; same-process noise is ~+-0.1 fps.
7. **Check correctness broadly**: verify-mode mismatches = 0, screenshots, a second scene, a long
   scripted playtest, the test suite.
8. **Keep or delete.** If it is not clearly faster, remove the code (do not accumulate default-off
   knobs). If it is, make it unconditional. Record the result, including failures, in an experiment log.

## Rules that saved time

- **Logic-only optimizations.** Every change must name the work or wait it provably removes, for
  the whole game. No tuned thresholds or per-scene knobs; they don't transfer and hide the real cause.
- **Hit rate is not savings.** A cache with 91% hits lost 3.5%; a memo with 10.7K hits/frame was
  slower. Count the full cost of the lookup, proof, invalidation and fallback path.
- **Moving work to another thread rarely helps** when items cost 1-5 us: hand-off costs 0.3-1 us per
  item, plus lock contention and lost cache locality (render split: flat).
- **Diagnostics must cost one relaxed load when off**, and instrumented fps is never a result
  (timed scopes added +32% cost).
- **Default to the console's timing.** Vblank must stay at the console's 60 Hz; a faster virtual
  vblank sped up the game clock (cutscenes 4x, gameplay slow-motion).
- **Suspect hardware for random host crashes** on a machine that also crashes the compiler; pin
  work away from bad cores before debugging.
- **Protect user saves.** Benchmarks run on a fixed baseline save swapped in and out with hash
  verification; never run experiments on the player's own save.
- **One emulator instance at a time**; background scripts kill only the process they started.

## Directions worth trying (ranked by evidence)

Proven large wins, apply first if the emulator lacks them:
1. **Record the Vulkan command stream on a worker thread and defer submits** (translation thread only
   encodes): 21.1 -> 23.7 fps; drains 191 -> 5 per frame.
2. **Native replay records for repeated draws** keyed by shader address + user data, holding
   flattened resource tables in an upload ring and a write-once descriptor set; per-draw cost 0.25 us:
   24.9 -> 30.9 fps. Store a record only on the second sighting (keys new every frame: +6.6%).
3. **Readbacks that don't wait for the whole GPU tail**: copy on a transfer-only queue and wait only
   for the last writer of those bytes / the already-completed timeline value (+1.25 fps; sync
   downloads 1.19 -> 0.29 ms/frame). Most guest readbacks read data finished frames ago.
4. **Cheaper page tracking**: batch/async reprotect, 64 KiB "resource here" bitmaps, per-page
   invalidation epochs, lock-free watched reads, release locks before mprotect in the fault handler
   (+1.1 fps from that alone), partial dirty ranges for huge texture pools.
5. **Barrier dedupe** between dispatches with no work in between (-330 of 920 barriers, +1.4%).
6. **Build**: -O3, ThinLTO, PGO (+8.4% with AOT-compiled resource-table plans), a JIT for linear
   resource-table evaluation. Ship a portable x86-64-v3 build; -march=native leaks GFNI/VNNI.

Stutter:
7. **Build missing pipelines unoptimized first** (VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT, ~150x
   faster on NVIDIA), compile the optimized one in the background and swap: cold stalls 22 s -> 0.
8. **Static precompile from the game files**: enumerate every shader the game ships (bundles + shaders
   embedded in the executable), derive their compile keys, compile offline into a pipeline cache keyed
   by GPU+driver; translate the rest in background threads at startup (prefetch).
9. **Fix translator complexity** (a dominator tree instead of iterating dominator sets: worst shader
   862 -> 77 ms) and make SPIR-V deterministic (hash-order ids made caches never hit).
10. **Never re-create big resources on partial unmap**: keep texture pools alive, upload only mapped
    parts, pool scratch allocations (a 2.5 s freeze came from a new VMA block per detile).

Open, promising (not done in the source project): batching runs of the same compute shader into one
dispatch (~2 ms estimated), async compute for full-screen post passes, host-pointer rings
(VK_EXT_external_memory_host) to avoid mprotect, fast paths for plain 32-bit formatted buffer loads,
persisting background-translated SPIR-V between launches.

Tried and not worth it (details in lessons.md): per-draw preparation reuse, render-thread split,
native replacement of culling/shadow/geometry chains, snapshotting writes for readbacks, shader
objects, DX12, pipeline libraries, strict thread pinning, skipping readback copies.

## Recurring bug classes to check first

- **Stale ranges after guest unmap**: caches, records and uploads that still read an unmapped tail
  (host access violation). Track unmaps with an epoch and re-validate.
- **Lost write protection**: partial unmap/remap restoring the original page access; untracked
  guest writes show up as frozen or detached models ("out-of-body").
- **Lock recursion**: re-acquiring a shared lock (SRW/shared_mutex) behind a waiting writer deadlocks.
- **Stale GPU state across mixed fast/slow paths**: bump the state generation when a fast path
  bypasses the normal binder (black silhouettes, stray triangles). Verify modes miss this; screenshots don't.
- **Queue ownership/slot reuse across queues**: bytes from another window returned ~every 2 s.
- **Windows specifics**: exception dispatch clobbers the guest SysV red zone (patch guest code at load);
  NVIDIA vkQueuePresentKHR holds a device lock (no wait-before-signal pending on the recorder, own present queue).
- **Nondeterminism** (pointer-keyed maps) breaking cache keys.

When a crash is reported: get host frames (RtlVirtualUnwind or backtrace), the fault address' page
state and the guest registers, symbolize with the build's linker map, then reproduce with the
save/route that triggers it. Guessing from the symptom produced one wrong fix in the source project;
the host frames found the real one immediately.
