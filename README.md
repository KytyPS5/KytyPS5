# KytyPS5-nhl

A private working fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5) focused on getting **EA Sports NHL 27**
(PPSA34063, v01.003.001) and **NHL 26** (PPSA26785) running well. The working branch is `nhl26`.

> [!IMPORTANT]
> Not affiliated with Sony Interactive Entertainment, PlayStation or Electronic Arts. No game files or
> system software are distributed. Use only game files you have obtained legally.

## Status (2026-10-10)

| | NHL 27 | NHL 26 |
|---|---|---|
| Boots to menus | Yes | Yes |
| Franchise / Play Now menus | Yes | Yes |
| Loads into a match | Yes | Yes (slow) |
| Player / scene lighting | **Fixed** (front-face encoding) | **Fixed** (same fix) |
| Commentary audio | Working (Opus multistream fix); some lines still garbled | – |
| Save / profile | Working | – |

## Performance

FPS ranges in normal play mode (no profiler or debug logging), measured after the lighting fix.

| Scene | RX 6800 XT, Ryzen 5 5600 | RTX 3070, Ryzen 7 3800XT, 16 GB DDR4-3600 |
|---|---|---|
| Main menu | 14-19 | 10-13 |
| Franchise menus (game caps at 30) | 26-29 | 19-21 |
| Team select / versus (players shown) | 7-12 | 8-9 |
| In match | 4-6 | 1-5 |

The menus are mostly limited by CPU-side per-draw preparation (descriptor/resource setup).
Gameplay also has heavy GPU passes. The goal for menus is a stable 30 fps.

## Known bugs

- **Jersey cloth glitches** in menus when jerseys move or flow quickly.
- **Some skin tones look wrong** (seen on a camera pan to the bench).
- **Wrong player model on the Franchise jersey-select screen**: with VAN vs EDM, the EDM side shows a
  Vancouver player (#17 Hronek, VAN home jersey) instead of an Edmonton player.
- **Black band above the bottom ticker** on the Franchise hub: the final composite pass
  (cs `40d1ee8b401c8219`) cuts the backdrop off below ~88% of the screen height. Not yet compared with the real console.
- **Occasional host crash** while navigating Franchise menus (access violation in a `memcpy`; the caller isn't identified yet).
- **Some commentary lines garbled**: the Opus decoder rejects valid packets with more than 6 frames and 40/60 ms frames.
- **RenderDoc capture mode** (`--rd`) currently ends in `VK_ERROR_DEVICE_LOST`.
- NHL 26: two GPU persistent-thread compute shaders (`3b25cdb347182b6d`, `613d940c02920148`) hang the GPU and are
  skipped (`KYTY_DBG_SKIP_CS`). The likely cause is GDS ops ignoring the M0 base.

## Notable fixes in this fork

- **Lighting (both games):** the pixel-shader front-face input now follows `SPI_BARYC_CNTL.FRONT_FACE_ALL_BITS`
  (integer 1/0 instead of float ±1). The lighting's `== 0` test never passed before, so direct light was
  never applied (players and floor were dark). Found by Cryan.
- **Commentary audio:** AJM Opus (codec 24) with a 2-byte LE length prefix. 2-channel instances are two uncoupled
  mono streams (self-delimited stream 0 + stream 1) whose TOC configs can differ.
- **Shader recompiler:**
  - partial-wave reductions (float-compare exec-bounded);
  - DPP/permlane inactive-lane preservation;
  - workgroup buffer sync;
  - bounded `S_SETPC` jump tables;
  - loop cap for runaway lighting loops (38 ms → 1.7 ms per draw);
  - memoized exponential recursion in resource materialization.
- **GPU / renderer:**
  - native indirect draws;
  - read-only buffer and BDA dirty caches;
  - program resource cache;
  - D16 depth sampled as R16;
  - D32 placeholders for depth-compare null slots;
  - BC6H storage-write redirect;
  - ignored LS/ES RSRC registers.
- **AMD driver crash:** Kyty's 3D detiler shader used an `OpSpecConstantOp Select` chain that crashes amdvlk.
  It is rewritten as if/return.
- **NHL 26 boot:** `param.json` missing from dumps (use `target-param.json`), and `--redzone` to stop host exception
  dispatch from clobbering the SysV red zone.

## Running NHL 27

Build `kyty_emulator` (see the upstream build instructions below), then run:

```powershell
kyty_emulator.exe --game "<path>\PPSA34063-app0\eboot.bin" --screen-width 1920 --screen-height 1080 --redzone
```

Recommended environment variables (all are needed for current performance and stability):

```
KYTY_DBG_LOOP_CAP=1024
KYTY_DBG_SKIP_BAD_SHADERS=1
KYTY_PIPELINE_CACHE_DIRTY=1
KYTY_EOP_DIRECT_LABELS=1
KYTY_NATIVE_INDIRECT=1
KYTY_READONLY_BUFFERS=1
KYTY_CLEAR_REGISTER_WIDE=1
KYTY_PARTIAL_WAVE_REDUCTION=1
KYTY_DPP_PRESERVE_INACTIVE=1
KYTY_WORKGROUP_BUFFER_SYNC=1
KYTY_BDA_DIRTY_CACHE=1
KYTY_PROGRAM_RESOURCE_CACHE=1
KYTY_PERF_NO_SYNC_READBACK=1
```

Useful debug options:

| Variable | What it does |
|---|---|
| `KYTY_PERF_GPU_TIMING=1` | Per-shader GPU timing (`NHL27GPU` lines); slows the game |
| `KYTY_DBG_FRAME_DUMP=1` | Press **F2** to dump the next frame to `captures/frame_N` (command list, image stats, buffers) |
| `KYTY_DBG_FRAME_DUMP_BUF_PS=<ps hashes>` | Also dump buffers, 3D images, SRT and push constants for those pixel shaders |
| `KYTY_DBG_TRACE_CS=<cs hashes>` | Log bindings for those compute dispatches |
| `KYTY_DBG_TRACE_WRITES=<addrs>` | Log GPU writes to those guest addresses |
| `KYTY_DBG_DUMP_SHADERS=<hashes>` | Dump shader binaries and SPIR-V |
| `KYTY_DBG_SKIP_CS` / `KYTY_DBG_SKIP_PS=<hashes>` | Skip shaders, for A/B tests |
| `KYTY_DBG_FORCE_SHADOW_COMPARE=always` | Force every depth-compare sampler to "lit" (diagnostic) |
| `KYTY_DBG_DEPTH_ALIAS=1` (+`_ADDR=`) | Log depth/texture aliasing events |

## Credits

- [KytyPS5](https://github.com/KytyPS5/KytyPS5) and the original [Kyty](https://github.com/InoriRus/Kyty).
- Cryan ([GitColeS](https://github.com/GitColeS)): front-face lighting fix and testing.
- AnyPS5 developers, for comparison data on the lighting defect.

## Building

Build exactly as upstream KytyPS5: Windows with clang-cl + Ninja + Qt 6. See the
[upstream README](https://github.com/KytyPS5/KytyPS5#building). Quick version (Windows, x64 Native Tools prompt):

```powershell
git submodule update --init --recursive
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows --target kyty_emulator
```

## License

KytyPS5 is licensed under the [GNU General Public License version 2](LICENSE) (`GPL-2.0-only`).
It is based on the original [Kyty](https://github.com/InoriRus/Kyty) (MIT). Kyty's notice is preserved in
[`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt). Third-party components remain under their own licenses.
