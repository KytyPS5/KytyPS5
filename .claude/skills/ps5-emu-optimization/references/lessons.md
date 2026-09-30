# Lessons: measured wins and failures

Source project: KytyPS5 fork running Demon's Souls (PPSA01341, v01.007.000), 2560x1440, i9-14900K
(HT off, 24 logical CPUs) + RTX 5090, Windows and Linux. Switch names are that fork's `KYTY_*`
environment variables; the ideas carry over to any HLE PS5 emulator.

## Frame anatomy at ~40 fps (before looking for new work, compare yours)

- Render (translation) thread ~20 ms/frame, 84% busy. GPU busy 16-19 ms but idle while waiting for
  translation. Game CPU threads alone: ~100 fps headless.
- Render thread split: compute dispatch prep ~8 ms (~4000 dispatches, 85% in runs of the same
  shader; resource-table evaluation 1.5 ms, buffer rebinding 1.7 ms, linear-copy detection 0.75 ms),
  native draw records ~5 ms, ordinary indexed draws ~3.6 ms (~970).
- ~183 vkQueueSubmit per frame. Frame start: previous frame's GPU tail 4.6 ms -> culling submit ->
  culling translation 3.3 ms -> culling GPU 1.9 ms -> main submit (10.2 ms chain).
- Guest threads wait 5.8-7 ms after each flip for readbacks queued behind the previous GPU tail.
  Readback sources: particle statistics ~53%, GPU culling ~32%, foot IK collision ~5%, auto exposure ~2%.
  Gameplay logic itself never read GPU results.
- Page tracking churn: ~2,950 pages/frame cycle upload -> reprotect -> fault; ~620 mprotect/frame
  at 3.9 us (~8% of the render thread, 60% of it kernel VMA split/merge).
- Causal probe: +1.2 ms on graphics-queue translation -> +1.06-1.23 ms frame time; the same delay on
  async compute queues -> +0.06 ms.

## Wins

| Change | Result | Why it worked |
|---|---|---|
| Vulkan recording worker + deferred submit | 21.1 -> 23.7 fps; drains 191 -> 5/frame | translation no longer waits on driver calls |
| Native draw records (replay repeated draws from per-object records) | 24.85 -> 30.93 fps; render 34.0 -> 25.9 ms | removes per-draw table evaluation and resource prep; emit 0.25 us/draw |
| Store records only on second sighting | 34.83 -> 37.13 (+6.6%) | ~550 keys/frame were new every frame and stored for nothing |
| Logic-only bundle (range-aware backing-store fence, lock-free watched reads, local interval splice, range-set hints, single-writer counters, depth partial upload, ...) | 36.08 -> 40.71 fps | many small provable removals of locks/waits; lock waits 12/frame 310 us -> 0.4/frame 3 us |
| Readback on a transfer-only queue, waiting only for completed timeline values | 34.80 -> 36.05 fps; sync downloads 1.19 -> 0.29 ms/frame | 83% of async readbacks read data finished frames earlier |
| Async readback for guest writes to GPU pages | 33.0 -> 34.5 fps | sync downloads 12 -> 6/frame |
| Release the resource lock before mprotect in the write-fault handler | +1.1 fps | render thread spun ~300 times/frame on that lock |
| Detach readbacks | +0.56 fps | |
| 64 KiB "image here" granule bitmap | +0.47 then +0.27 | cheap negative lookups |
| Prefetch next draw record | +0.50 | cache misses on record data |
| Reprotect on a worker thread | +0.25; render mprotect 608 -> 320/frame | |
| Narrow readback window | +0.20; sync downloads 10 -> 5.4 | |
| Global barrier dedupe | +1.4% (-330 of ~920 barriers) | barriers with no work between them |
| Record ordinary draws as packets | -1.3 ms render | they were silently excluded |
| Image recycle pool | -1 ms | |
| -O3 -march=native + ThinLTO | ~+4% | |
| PGO + AOT-compiled resource-table plans | +8.4% (AOT alone ~1.5%) | |
| Non-exclusive pin of the render thread to P-cores (not CPU 0) | +2.5% | Windows interrupts land on CPU 0 (pinning to it halved fps) |
| Unoptimized-first pipelines, optimized in background | cold stalls 22 s -> 0; 113 CS 89 s -> 0.6 s | driver optimization is the expensive part |
| Dominator tree in CFG structurization | worst shader 862 -> 77 ms; route stalls 2.7 s -> 1.4 s | quadratic dominator-set recomputation |
| Background translation of all game shaders at startup | slow translations 44/916 ms -> 5/299 ms on a cold route | the remaining misses are specialization guesses |
| Streaming fixes (keep pools on partial unmap, dirty ranges only, binary search in interval queries, pooled detile scratch, no full GPU finish on unmap) | worst frame 200 -> ~113 ms; worst 1-s window 21 -> 29-30 fps | re-creation and O(n) scans under streaming |

Title-specific guest patches (exact title + version match) also paid: omitting barriers between
compute dispatches +12.7%, a memmove fast path +5%, spin-to-sleep (CPU 1338% -> 388%). Keep them
separate from the generic emulator and verify the patched bytes before writing.

## Failures (do not retry without a new argument)

| Idea | Result | Reason |
|---|---|---|
| Reuse full per-draw preparation (PBR) | -3.5% / -2.3% | 1.1-1.6 ms bookkeeping despite 91% texture hits |
| Per-program stage reuse | -0.4 fps | saved 1 ms, bookkeeping 0.77 ms plus misses |
| Repeat-draw fast paths (6 variants) | slower or equal | the 5.2 ms upper bound came only from skipping correctness work |
| Buffer memo | 2.0 ms bookkeeping for 1.05 ms saved | |
| Same-program dispatch-run buffer memo | slower despite 10.7K hits/frame | real cost was reprotect ping-pong, not lookup |
| Native records for compute dispatches | 21 fps | culling tables relocated every frame (ring allocations) |
| Render thread split into two stages | flat (31.1-31.5 vs 30.9-31.4) | hand-off 0.3-1 us/item vs 1-5 us work; mutex contention 1.5 ms |
| Native shadow pass / native culling / GPU geometry chains / whole-segment culling | -10% to -35% | memory reuse fails ~2500 overlap proofs/frame; proof + fallback cost |
| Compute packet merge, descriptor defer, constant merge | -0.1 to -1 fps | |
| Snapshot guest writes for readbacks | async readbacks 62 -> 19 but -1.25 fps | two barriers per snapshot |
| Skip readback copy | 9 fps | broke the ownership model |
| Latent / zero-copy readback feedback | -8%, crashes | |
| Consume GPU indirect args directly | no change | |
| Wider CPU write windows (16/64 pages) | slower, upload x4 | |
| Leave hot pages unprotected | slower guest | |
| Second async queue for the frame-start chain | <= 0.6 ms | true data dependency |
| Unprotect on the guest thread | 9 s freeze | |
| Strict pinning of render/record/upload threads | 28 fps (worse) | |
| VK_EXT_shader_object | slower warm (3582 vs 1358 ms), broken | |
| Pipeline libraries / more dynamic state | <= 3% possible | |
| DX12 backend | not recommended | no shader pointers; bottleneck is CPU translation |
| Patch the game's fps setting | caps nothing | frame pacing comes from the flip queue |
| Faster virtual vblank (240 Hz) | game clock wrong | keep 60 Hz |
| Shared out-of-line format decode in shaders | no compile gain, driver compiler crash | |
| Ten small switches retested together | -0.27 to +0.43 ms each, none distinguishable | delete, don't keep |

Diagnostic ceilings (not optimizations, useful for "how far can we go"): volumetric fog off
40.30 -> 42.15; sun cascade shadows off 40.60 -> 44.05; fog + three shadow types off 40.43 -> 47.07.

## Shader/pipeline facts for one AAA title

- ~20.6K programs (10.7K compute, of which 9.5K particle variants), ~50K pipelines; full static
  precompile ~16 CPU-hours of driver compile (44 min on 22 threads, 8 processes x 3 threads, because
  NVIDIA compiles big compute shaders nearly serially inside one process). Cache ~2.3 GB.
- Runtime coverage of statically derived keys: CS 82%, PS 91%, VS 100%; misses are specialization
  guesses (uint textures, storage swizzles, cube/2D bound to a declared 2D array, written buffer formats).
- Compile-cost multipliers: wave64 on a 32-wide GPU emitting every instruction twice (x2.3 compile),
  runtime format decode as nested switches per component (up to 9.5 MB SPIR-V, 107 s), EXEC
  predication (~4K selects, ~11K bitcasts per big shader). 77% of formatted buffer loads were plain
  32-bit formats: a fast path candidate.
- Cold-compile tests are confounded by the driver's own disk cache and by the emulator's local
  cache; compare module coverage, not stall counts.
