---
name: kyty-gpu-hang
description: Diagnose a KytyPS5 hang where the window freezes, the present count stops, or the GPU never finishes a submit. Uses the autoplay HANG classification, the heartbeat's GPU tick and recent submits, gdb backtraces, the GPU submit trace and Vulkan validation. Use when a run is classified HANG (exit 12) or the emulator stops responding.
---

# KytyPS5 GPU hang

A hang is **the host present count staying flat while the process is alive**. `kyty_autoplay.py run`
detects it (20 s after the first present, `--boot-grace` before it), saves the evidence, quits the
emulator and exits 12. This skill is how to read that evidence.

The original `.cursor/skills/kyty-gpu-hang/` was not available when this was written, so this is
built on the tooling that exists rather than ported from it; keep that skill for anything it covers
that this does not.

## Evidence in the run directory

- `summary.md`, section **Hang**: GPU tick versus current tick for each command scheduler, the most
  recent submits, the last shader sent to the compiler.
- `gdb.txt`: `info threads` and `thread apply all bt 40` of the live process, taken before it was
  asked to quit. The emulator allows ptrace attach itself (`PR_SET_PTRACER_ANY`), whatever
  `kernel.yama.ptrace_scope` says.
- `status.jsonl`: heartbeat history; see when `host_presents`, `guest_flips` and `pad_reads`
  stopped moving, and in which order.
- `_kyty.txt`: guest log, flushed per line.

## Reading the heartbeat

| what you see | meaning |
|---|---|
| `gpu_tick < current_tick`, both flat | the GPU stopped finishing submitted work. `gpu_behind_by` is how many submits are outstanding. Look at `recent_submits` for what was last submitted |
| `gpu_tick == current_tick`, presents flat | the GPU is idle; the CPU side is waiting. Go to the backtraces |
| `guest_flips` flat, `host_presents` flat, `pad_reads` still rising | the guest is running but not flipping: look for the guest waiting on an event (a flip, an end-of-pipe interrupt) in the backtraces |
| `pad_reads` flat too | the guest threads are blocked or spinning in something else |
| heartbeat itself stopped (`uptime_ms` flat) | the whole process is frozen (SIGSTOP, a hold in the kernel); `gdb.txt` may be all that is left |

`recent_submits[].op_name` is the last operation recorded into that command buffer
(`DispatchDirect`, `DrawIndex`, `DrawIndexAuto`, `EopWrite`, `EopInterrupt`, `EopWriteBack`,
`EopFlip`, `EopWriteBackFlip`, `EopOnlyFlip`, `DispatchIndirect`); `args` are the op's arguments as
recorded by `CommandBuffer::SetDebugInfo`. A hang right after an `Eop*` op that should signal a flip
points at the end-of-pipe path; one after a dispatch or draw points at that work.

## Backtraces

Find the threads that matter before reading individual frames: the thread blocked in
`vkWaitSemaphores`/`MasterSemaphore::Wait` (the CPU is waiting for the GPU), the thread inside
`Presenter::Present` or the swapchain (`vkAcquireNextImageKHR`/`vkQueuePresentKHR`), and guest
threads waiting in a kernel event queue or semaphore. Note which lock each holds or waits on: the
present path takes `present_mutex` and then the render lock, and holds `present_mutex` while a
screenshot waits for the GPU.

## Narrowing it down

Reproduce with more information, one switch at a time (each slows the emulator). Pass emulator
options to `run` or `start` with `--args="..."`:

1. `kyty_autoplay.py start ...` then `send "trace on"`: logs every GPU submit (tick, GPU tick, op,
   args) to `_kyty.txt`. Or pass `--args="--graphics-debug-dump true"` for the same plus the
   emulator's other graphics dumps.
2. `--args="--vulkan-validation true"` for validation-layer errors; add
   `--gpu-assisted-validation true` to bounds-check shader accesses on the GPU (very slow). A shader
   that reads or writes out of bounds is a common cause of a device that stops making progress.
3. `--args="--shader-validation true"` rules out invalid SPIR-V from the recompiler. The captures
   of the shaders in flight are in `<run>/capture`; `--shader-replay` each one that was compiled
   shortly before the hang (the heartbeat's `last_shader`).
4. `--args=--rd` enables RenderDoc capture.
5. `stall_present <seconds>` (an automation command) freezes presentation on purpose; use it to
   check that hang detection and the evidence collection work end to end before blaming the game.

A `vkQueueSubmit failed` line in the log is printed with the tick and the debug op and arguments of
the failing submit (`ReportVulkanFatal` in `commandScheduler.cpp`); that is a device loss, not a
hang, and shows up as OTHER_ABORT.

## Reporting

State: which of the heartbeat patterns above applied, the last ops and shader, the thread that was
blocked and on what, and what the extra switches showed. A hang with no new evidence after these
steps is not diagnosed; say so rather than guessing.
