# Performance report

The performance report is a plain-text summary of where the emulator spends time on the CPU,
written every 5 seconds while a game runs. It complements the Tracy profiler: Tracy shows what
each thread was doing, and the report answers *which code path* made the CPU wait for the GPU or
submit work, and *which guest memory* caused write-tracking faults. These are the questions that
usually explain a low frame rate.

## Enabling it

- **Launcher:** game settings → *Debugging & logs* → **Performance report**, with the file in
  **Performance report file**.
- **Command line:** `--perf-report`, and optionally `--perf-report-file <path>`.

The report is written to `_kyty_perf.txt` by default. A relative path is relative to the
emulator's working directory, as for the printf output file. The report doesn't depend on the
logging settings, and the file is replaced each time the emulator starts.

When the option is off, the instrumentation costs a relaxed atomic load and a branch per event.
When it is on, each GPU wait, queue submit and fault takes a lock to be recorded. In testing, the
frame rate with the report on was within run-to-run noise of the frame rate with it off.

The GPU command thread writes each block between commands. While it is stuck in one long
command an interval stretches, so check the interval length printed on each block's first line.

## Reading it

Each block covers one interval. Rates are per second of wall time. `ms/s` is time spent per
second, so `1000 ms/s` keeps one thread fully busy. Here is an abridged block from a game's main menu:

```
=== 82.4 s (interval 5.1 s)
  frames: 3.7/s
  GPU command thread: idle 4.7%, blocked on guest waits 0.0%
  GPU waits: 1510.4/s, 136.2 ms/s
  queue submits: 1775.1/s, 25.3 ms/s
  write-tracking faults: 16133/s, 385.4 ms/s in the handler (buffer cache 312.6 ms/s, texture cache 1.4 ms/s)
    read faults: 559/s, 71.4 ms/s
    faults that downloaded GPU data first: 1516/s, 309.9 ms/s
    pages: 2/s faulting for the first time, 11296/s faulting again within an interval
    by thread (19): 3190/s 73.5 ms/s; 504/s 54.3 ms/s; 1136/s 31.4 ms/s; 1136/s 30.6 ms/s;
  tracking protection changes: 17121/s, 20.9 ms/s waiting for the address-space lock, 23.9 ms/s changing protection
  buffer uploads: 559/s, 58.3 MB/s
  BDA synchronization queued: 16541/s, 84.0 MB/s
  buffer cache: 1418 buffers, 5004.6 MB; 3.7 GC ticks/s
  GPU waits by call site:
      1501.7/s     98.4 ms/s  gpu   bufferCache.cpp:238 bool __cdecl Libs::Graphics::BufferCache::DownloadBufferMemory(...) [async = false]
         7.4/s     37.7 ms/s  other (outside an instrumented scheduler call)
  queue submits by call site:
      1503.2/s     20.5 ms/s  gpu   bufferCache.cpp:238 bool __cdecl Libs::Graphics::BufferCache::DownloadBufferMemory(...) [async = false]
       262.9/s      4.6 ms/s  gpu   graphicsRun.cpp:240 void __cdecl Libs::Graphics::CommandProcessor::BufferFlush(void)
         3.7/s      0.0 ms/s  gpu   graphicsRun.cpp:1313 void __cdecl Libs::Graphics::CommandProcessor::Flip(void)
  write-tracking faults by 1 MiB region:
       133.5/s     57.3 ms/s  0x117b600000
        70.1/s     31.1 ms/s  0x1176900000
        ...
  download faults by the shader write that owns the page:
       396.6/s    158.2 ms/s  write in 0x1171258040+0xe7fffc0 written by shader 0x11aabeec56222407
       342.3/s     44.5 ms/s  write in 0x1137c90000+0x1980000 written by shader 0xfe937637dd9037bf
        ...
```

In this example the game presents 3.7 frames per second and the GPU command thread is almost never
idle. Nearly every GPU wait and queue submit comes from synchronous buffer downloads, and the
faults behind them repeat on the same pages within each interval: guest code keeps writing to
pages that shaders also write (for example shader `0x11aabeec56222407`), so every write
downloads the GPU's data and waits for the GPU first. Avoiding that round trip for those buffers
is where to look.

| Line | What it measures | What to look for |
|---|---|---|
| `frames` | Completed video-out flips. | The frame rate the game is presenting. |
| `GPU command thread` | Share of the interval the GPU command thread had no work (*idle*), or had work but every queue waited on a guest condition such as `WAIT_REG_MEM` (*blocked*). | Near 0% idle with a low frame rate means the command thread is the bottleneck: read the rest of the block. High *blocked* means it is waiting for the game's CPU threads. |
| `GPU waits` | Times a CPU thread blocked on the GPU timeline semaphore, and the total time. | Every wait stalls the CPU until the GPU finishes all earlier work. See *by call site* below. |
| `queue submits` | Vulkan queue submissions and the time spent submitting. | Many submits per frame usually come from synchronous downloads or flushes. |
| `write-tracking faults` | Guest writes to memory the GPU caches track, and the time spent in the fault handler. | The handler runs on the faulting game thread, so this time blocks the game directly. |
| `faults that downloaded GPU data first` | Faults on pages the GPU had written: the handler waited for the GPU thread to download them before the write could continue. | Usually the most expensive kind. See *download faults by the shader write* below. |
| `pages` | Pages faulting for the first time vs. faulting again in the same interval. | Repeat faults mean memory ping-pongs between CPU and GPU ownership. |
| `tracking protection changes` | Page-protection changes made by write tracking: time waiting for the address-space lock, and time in the OS call. | High call time means many small protection changes. |
| `buffer uploads` | Guest memory copied into GPU buffers. | |
| `BDA synchronization queued` | Page ranges queued for buffer-device-address synchronization. | |
| `buffer cache` | Buffers and bytes in the buffer cache, sampled at each garbage-collector tick. | Steady growth suggests buffers are not being reused or released. |
| `GPU waits by call site` / `queue submits by call site` | The same events, attributed to the code that called the command scheduler (`Flush`, `FlushAndWait`, `Finish`, `Wait`, `Submit`). `gpu` / `other` is the thread that waited. | The top entry is normally the code path to fix. |
| `write-tracking faults by 1 MiB region` | Faults grouped by guest address. | Which guest allocation is involved. |
| `download faults by the shader write that owns the page` | Download faults matched to the most recent GPU-written buffer binding covering the page, with the hash of the shader that wrote it. | Use the shader hash with shader dumps to find what the GPU writes and the game reads back. |

The top lists show the 15 most expensive entries by time and are reset each interval.

## Adding a measurement

The instrumentation lives in `src/common/perfReport.h` and `src/common/perfReport.cpp`:

1. Add a `Note*` function. Return immediately when `!Enabled()`. Use the relaxed atomics in
   `State` for simple counters, and `State::lock` for maps.
2. Call it at the event. To time something, take `const auto start = Common::PerfReport::Now();`
   before it and pass `start` to the `Note*` call. `Now()` returns 0 when the report is
   disabled, and the `Note*` calls ignore a zero start.
3. Print it in `WriteInterval` and reset it there. Describe the line in the table above.

Fault handlers run on guest threads inside the host exception handler. Keep their `Note*`
calls free of guest memory accesses.
