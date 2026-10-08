# Frame pacing and measurement

VideoOut uses absolute vblank deadlines with fractional-clock compensation.
Small wake-up errors preserve cadence; a stall of a full additional period drops
old timing debt. The configured frequency and guest flip interval remain the
timing inputs. FG pacing adds display frames without changing simulation FPS.

The window title updates asynchronously once per second. New guest frames,
cached/blank presentations and SDK display frames have separate counts. The
title's FG FPS is the SDK's reported throughput, not measured scanout.

## Capture

```powershell
$env:KYTY_FRAME_TIMING_CSV = "$PWD/_Build/frame-timings.csv"
& _Build/windows/kyty_emulator.exe --game 'path/to/eboot.bin' --dlss Off --vblank-frequency 60
Remove-Item Env:KYTY_FRAME_TIMING_CSV
python tools/analyze-frame-timings.py _Build/frame-timings.csv --start 35 --end 60
```

Recording is disabled unless the variable is set. Files are buffered; close the
emulator normally before analysis. The analyzer ignores incomplete trailing rows.

| CSV field | Meaning |
| --- | --- |
| `frame_ms` | CPU interval between successful presentation submissions |
| `present_ms` | Acquisition, recording, submit/present and title scheduling |
| `new_frame` | New guest frame, excluding cached refreshes and blank frames |
| `dlss_evaluated` | Successful SR evaluation, including OptiScaler |
| `display_frames` | Backend-reported frames for a new MAIN presentation |
| `prepare_wait_ms`, `prepare_lock_ms` | Producer retirement and renderer lock |
| `resolve_ms`, `inputs_ms` | Source resolution and input preparation |
| `fg_capture_ms`, `dlss_record_ms` | FG snapshots and SR recording |

All these times are CPU measurements. They do not isolate GPU execution.
`present_ms` excludes the producer's earlier SR work and Fsr FG's deferred
spacing wait. Cached/blank frames have zero preparation timings. Repeated
presentation of an evaluated frame is not another SR evaluation or guest frame.
Older CSVs without optional fields remain supported.

## Compare

Use the same executable, GPU/driver, scene, input sequence, guest version,
resolution, render scale, vblank rate and power profile. Record the binary hash
and dirty-tree status. Exclude startup, shader compilation and transitions.
Alternate baseline/candidate runs; avoid captures and overlays during samples.

Analyze the same stable interval and report individual runs as well as aggregate
guest FPS, median/P95/P99 intervals and long stalls. Mean guest FPS counts frame
intervals divided by elapsed time. The first selected row establishes the time
boundary; its interval is excluded because it began before the sample window.
Its presentation-call duration remains part of the presentation statistics.

Verify that Off has no successful SR evaluations and that each eligible new
frame in an active run has one. Report guest submissions, SDK display counts and
GPU cost separately. Equal FPS at a vblank cap does not establish equal cost;
a higher-cap stress run changes guest timing and must be identified separately.
These counters do not prove that game logic or animation advances at that rate.

Keep local CSVs, images and reports in ignored `_Build/`. See
[dlss.md](dlss.md) for runtime requirements, quality limitations and GPU checks.

## Checks

```powershell
cmake --build _Build/windows --target dlss_gpu_tests frame_pacer_tests prepared_frame_selection_tests
ctest --test-dir _Build/windows -R '^(presentation_|prepared_frame_selection|frame_timing_analysis)' --output-on-failure
```

The pacer covers fractional refresh, oversleep, stalls and refresh changes.
The title test pauses UI event handling and verifies that frame updates do not
block the producer. GPU presentation checks cover reuse and queue dependencies.
