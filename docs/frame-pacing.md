# Frame pacing diagnostics

The window title updates once per second without waiting for the UI thread.
Previously every presented frame synchronously called the UI thread, so a busy
window event loop could stop presentation. Title callbacks own their text and
resolve the SDL window ID when they run, including after a window closes.
The FPS counter counts new guest frames; host blank frames and repeated paused
frames do not inflate gameplay FPS.

VideoOut uses absolute vblank deadlines with fractional-clock compensation.
Small wake-up errors retain the cadence. Delays of at least another full period
discard accumulated timing debt, preventing a long stall from producing a burst
of accelerated vblanks. The configured vblank frequency and guest flip interval
remain the timing inputs. Increasing that frequency is not a performance fix.

## Capture and compare

Enable a buffered CSV only for a diagnostic run:

```powershell
$env:KYTY_FRAME_TIMING_CSV = "$PWD/_Build/frame-timings.csv"
& _Build/windows/kyty_emulator.exe --game 'path/to/eboot.bin' --dlss Off --vblank-frequency 60
Remove-Item Env:KYTY_FRAME_TIMING_CSV
python tools/analyze-frame-timings.py _Build/frame-timings.csv --start 35 --end 60
```

Repeat with `--dlss Quality`, a different trace filename, the same scene, input
sequence and frequency. Separate startup/shader compilation and area transitions
from steady-scene samples. Do not compare menu samples to gameplay samples.

`frame_ms` measures the CPU interval between successful presentation submissions.
`present_ms` measures the call including image acquisition, recording, submission,
presentation and title scheduling. These are **not GPU execution or displayed
frame timings**. `new_frame=0` identifies repeated cached presentations and host
blank frames.

The counter records guest frame submissions, not whether successive guest
images contain distinct visual content. It cannot establish that game logic
or animation advances at the same rate as the configured vblank frequency.
`dlss_evaluated=1` means the frame contains an output successfully evaluated by
the backend; repeated presentation of that output is not a new evaluation.
The analyzer counts evaluations only on new frames.

Optional producer timings are recorded as `prepare_wait_ms`, `prepare_lock_ms`,
`resolve_ms`, `inputs_ms`, `fg_capture_ms` and `dlss_record_ms`. They separate CPU
frame retirement, renderer-lock acquisition, source resolution, input preparation,
FG snapshot recording and output/NGX recording. Cached/blank frames carry zero
preparation timings. The analyzer summarizes the preparation of new MAIN frames;
these values remain CPU measurements, not GPU timestamps.

`display_frames` records Streamline's count for each new MAIN presentation when
FG is enabled, or one for ordinary presentation. Cached refreshes contribute zero.
The analyzer reports `sdk_display_fps` separately from `guest_fps` and continues
to accept older traces without these columns.

Files are buffered. Close the emulator normally before analyzing; a forcefully
terminated process can lose the last buffered samples. The analyzer skips an
incomplete trailing row. CSV recording is disabled unless the variable is set.

With a requested DLSS mode the title also displays `DLSS: active` or
`DLSS: inactive`, according to the presented frame. The emulator-wide path now
generates temporal inputs from the main VideoOut color, so a supported surface
can activate reconstruction without a game-specific adapter. This stage adds
GPU work. The separate render-scale option reduces supported raster passes and
adds attachment copies; measure total cost separately. Frame Generation inserts
display frames, which the guest-submission CSV does not count. When FG is
enabled, the title's `fps` (tagged `[FG]`) shows the SDK's reported
display-frame throughput, including generated frames. This is an SDK
counter, not an independent scanout measurement.
See [dlss.md](dlss.md).

## Reproducible DLSS comparisons

Use the same executable, GPU, driver, guest dump/version, window size, screen
resolution, shader optimization, vblank rate, scene and input sequence in both
runs. Record the executable hash and whether the source tree is dirty. Capture
CPU/GPU model, RAM, OS build, GPU driver, AC/battery state and power profile.
Do not change a power profile between runs; note unmeasured thermal or
background-load effects instead of attributing all variation to DLSS.

Run both Off and an active DLSS mode at the normal vblank rate. If both reach
the cap, their equal mean FPS does not show equal rendering cost. An additional
pair at a higher, identical vblank setting can expose throughput differences,
but this is a stress run that can change guest timing, not proof of equivalent
gameplay at that FPS. Repeat in alternating order (for example Off, Quality,
Quality, Off) to expose variation between runs.

Analyze the same elapsed CSV interval after initialization and shader warm-up.
Verify the scene outside that interval; PrintWindow, desktop screenshots,
overlays and GPU validation can perturb measured runs. Avoid captures during
the sampled interval. Reject comparisons if scenes, options or binaries differ.
Earlier traces from another executable or with captures during sampling are
historical evidence, not the baseline for the current change.

Mean guest-submission FPS is `(new_frames - first_row.new_frame) / elapsed`,
where `elapsed` is the time between the first and last selected submissions.
For multiple runs, divide the total counted frame intervals by their total
elapsed time and also report individual means/range. Do not average reciprocal
per-frame intervals or mix Off and active frames into one mean.

Interval statistics exclude the first selected row: its `frame_ms` begins before
the selected window. The row still establishes the elapsed-time boundary and
contributes its own `present_ms`, which measures the presentation call itself.
This prevents a preceding area transition from becoming a steady-scene stall.
`frame_timing_analysis` checks both an excluded preceding transition and a
retained stall inside the window when a Python interpreter is available.

Report interval median/P95/P99 and long intervals alongside mean FPS.
`present_ms` excludes the earlier producer-side input generation and NGX
evaluation; it cannot isolate DLSS GPU cost. Verify that Off has zero successful
evaluations and that every measured new frame in an active run has one. Show
FPS over time using fixed-width bins and state the bin width. Label graphs as
CPU guest-submission throughput, rather than GPU execution or scanout FPS.

Keep machine-specific reports, CSVs, screenshots and plot outputs under
`_Build/reports/`, which is ignored by Git. Prepare the Markdown report for the
PR description/comment and upload its PNG there; relative local image paths
must be replaced by uploaded attachment URLs before publishing. Keep generic
implementation and measurement instructions in `docs/`. A menu/calibration
sample must be identified as such and cannot establish full-game performance
or an image-quality improvement.

## Focused checks

```powershell
cmake --build _Build/windows --target dlss_gpu_tests dlss_settings_tests upscale_menu_tests shader_cfg_tests frame_pacer_tests
ctest --test-dir _Build/windows -R '^(dlss_|presentation_|frame_timing_analysis$|shader_cfg$|upscale_menu$)' --output-on-failure
```

The pacer test covers fractional cadence at 360 Hz, a 100 ms stall, ordinary
oversleep and refresh changes. The SDL title regression test stops pumping the
UI thread while issuing 120 frame updates, checks periodic refresh, and verifies
that both paths let the producer finish without UI work.
