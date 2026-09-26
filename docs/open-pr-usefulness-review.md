Дата обзора: **8 сентября 2026 года** · полный переснимок: **26 сентября 2026**

# Открытые PR KytyPS5: что полезно для текущего bring-up

## Полный переснимок 26 сентября 2026 (113 open)

GitHub REST `state=open&per_page=100` (paginate) → **113** открытых PR
(range #148–#845, drafts=5).
Сравнение с `yotei-windows-bringup` @ `b2035d7c`+ (draft
[#497](https://github.com/KytyPS5/KytyPS5/pull/497), base `main` tip `fd2e15ee`).

**Runtime на tip:** первый ненулевой кадр (spinner) доказан
`…-081515-presentfix-gpuav` prepared frame 250. Меню/gameplay **PENDING**.
Long run `…-081840`: watchdog **shown=159** — CS `54904fb4` 4× CreatePipeline
~287–295 с под GPUAV (4-й begin без done). EXIT large transfer **не** hit.

Это triage по title/files/метаданным всех 113 PR + deeper file look на
shader/GPU/Windows candidates. Не утверждение, что все 113 собраны и
прогнаны на GPU.

### Короткий вывод (актуально)

- Готового чужого PR «целиком → меню Yōtei» **нет**.
- **Merge 26.09 после `…-081840`:** ничего не влито в #497 — ни один open PR
  не снимает specialization churn `54904` без собственного RED / conflict
  resolve. `#718` CONFLICTING + cold no-op + revision-keyed; `#842–#845`
  только when EXIT observed.
- Лучшие **performance** кандидаты после correctness: **#718** (precompile
  recorded shaders — только warm 2nd+ boot, не текущий wall), **#628** (SRT
  memo, CONFLICTING), **#735** (uniform readlane elim, MERGEABLE но не
  CreatePipeline), куски **#506/#767** только после split + RED.
- Лучшие **correctness when observed**: **#845/#844/#842** (stage large
  upload/download вместо EXIT), **#537** (publish linear storage before CPU),
  **#638** (memory-pressure GC), **#793** (mesh draw split), **#761**
  (fragment-helper ballots) — только с собственным RED.
- **Не merge whole:** #833, #780, #767, #789, #512, #558, #555, #715, #654,
  #720, #747, draft #599/#613.
- **#811** image side уже selective на #497; re-merge не нужен.
- **#837** float image atomics: сначала доказать gap vs уже влитый #490 class.

### Уже в текущей ветке (не тащить повторно)

| Источник | Статус |
| --- | --- |
| [#497](https://github.com/KytyPS5/KytyPS5/pull/497) | **current:** tip after docs refresh; MERGEABLE vs `fd2e15ee`. |
| [#811](https://github.com/KytyPS5/KytyPS5/pull/811) images | **covered selectively**; buffer-key остаётся локальным RED. |
| Main `fd2e15ee` | audio/NGS2, translator void/`V_CMPX_O_F32`, MaxBuffers=64, lru_cache tests, CI recompiler tests. |
| Present/Vrr/HTile/readback 26.09 | soft-stall, GetVrrStatus, native HTile owner, post-EOP present readback. |

### Рекомендуемая очередь (меню)

1. Shared RED: specialization identity / SPIR-V size для CS `54904` class
   (`emulator-test-debt.md`) — главный блокер past spinner.
2. Повторить long run только после снижения CreatePipeline cost или с
   bound watchdog, достаточным на N permutations (~5 мин × N).
3. **#718** — только после conflict resolve + identity RED; не ожидать
   ускорения cold / текущего hung mid-compile.
4. Если hit EXIT на large upload/download — RED + selective **#842/#844/#845**.
5. Не трогать mega-bundles (#780/#833/#767) до точного shared RED.

### Каталог всех 113 open PR (26.09)

Обозначения: **P1** / **P2** / **covered** / **diagnostic** / **separate** /
**do not merge whole** / **test** / **draft**. Для PR ≤509 без новой пометки
сохранена оценка из снимка 8.09 (ниже); для ≥510 — оценка этого переснимка.

| PR | Draft | Author | Оценка | Title |
| --- | --- | --- | --- | --- |
| [#845](https://github.com/KytyPS5/KytyPS5/pull/845) |  | theantipopau | **P1 when observed**: oversized image readback staging; fail-closed EXIT→private buffer. Take only with RED for large readback abort. | gpu: stage oversized image readbacks in a private buffer instead of exiting |
| [#844](https://github.com/KytyPS5/KytyPS5/pull/844) |  | theantipopau | **P1 when observed**: oversized image upload staging; same class as #845 for uploads. | gpu: stage oversized image uploads in a private buffer instead of exiting |
| [#843](https://github.com/KytyPS5/KytyPS5/pull/843) |  | Cosmo | **separate**: DualSense haptics from vibration ports; not menu pixel path. | audio: play DualSense haptics from vibration audio ports |
| [#842](https://github.com/KytyPS5/KytyPS5/pull/842) |  | theantipopau | **P1 when observed**: batched buffer downloads instead of abort; relevant if Yōtei hits large download EXIT. | gpu: stage buffer downloads in batches instead of aborting on a large one |
| [#841](https://github.com/KytyPS5/KytyPS5/pull/841) |  | 6d4m | **separate**: AudioOut write serialization; not frame blocker. | audio: serialize blocking writes per output port |
| [#840](https://github.com/KytyPS5/KytyPS5/pull/840) |  | theantipopau | **test**: dormant VOP3 lane-read guards; useful neighbor coverage, no runtime claim. | tests: wire dormant VOP3 lane-read check and guard new dormant tests |
| [#839](https://github.com/KytyPS5/KytyPS5/pull/839) |  | ew-sudo | **P2/separate**: SSE4a EXTRQ trampolines; useful if guest hits SSE4a on host without it (#467 class). | loader: replace SSE4a EXTRQ with native trampolines on hosts without SSE4a |
| [#838](https://github.com/KytyPS5/KytyPS5/pull/838) |  | ew-sudo | **P2 Windows**: retry Windows file reads via bounce buffer on protected guest pages. | common: retry Windows file reads through a bounce buffer when the guest buffer is protected |
| [#837](https://github.com/KytyPS5/KytyPS5/pull/837) |  | theantipopau | **covered/check**: MIMG float FMIN/FMAX — production class likely already on bring-up; verify before port. | shader: support MIMG float image atomics (FMIN/FMAX) |
| [#836](https://github.com/KytyPS5/KytyPS5/pull/836) |  | theantipopau | **diagnostic P2**: guest fault → module+backtrace; helps long-run diagnosis. | loader: attribute guest faults to modules and print a backtrace |
| [#834](https://github.com/KytyPS5/KytyPS5/pull/834) |  | Pcniado | **separate**: exFAT raw image boot; not retail PKG path for Yōtei. | loader: boot titles from a raw exFAT image |
| [#833](https://github.com/KytyPS5/KytyPS5/pull/833) |  | hamzashakir99 | **do not merge whole**: Tekken 8 Windows stability bundle (cache/pipeline/pageManager); extract only with own RED. | Windows stability fixes found while bringing up Tekken 8 (PPSA10595) |
| [#827](https://github.com/KytyPS5/KytyPS5/pull/827) |  | Pcniado | **P2/separate**: munmap across reservation holes; kernel completeness. | kernel: allow munmap across released reservation holes |
| [#820](https://github.com/KytyPS5/KytyPS5/pull/820) |  | 6d4m | **covered?**: zero freshly allocated direct memory — may already be in tip via main; verify before port. | kernel: zero freshly allocated direct memory |
| [#818](https://github.com/KytyPS5/KytyPS5/pull/818) |  | leonardosth | **P2 Windows**: long paths + resource-tracking non-zero; mixed bundle, split first. | Support long paths on Windows and handle non-zero resource tracking o… |
| [#811](https://github.com/KytyPS5/KytyPS5/pull/811) |  | tototomate123 | **covered selectively**: address-backed images already selective on #497; buffer-key still local RED, not re-merge whole. | shader: handle address-backed indirect image descriptors |
| [#808](https://github.com/KytyPS5/KytyPS5/pull/808) |  | romainhedouin | **separate**: drop cpuinfo dependency; build hygiene. | common: drop the cpuinfo dependency |
| [#799](https://github.com/KytyPS5/KytyPS5/pull/799) |  | theantipopau | **covered?**: CFG stage/hash in failures — check if already in tip. | Report the shader stage and hash in CFG build failures |
| [#797](https://github.com/KytyPS5/KytyPS5/pull/797) |  | theantipopau | **P2/separate**: Windows MSG_PEEK/WAITALL; network/tests. | Fix Windows MSG_PEEK and MSG_WAITALL receive handling |
| [#795](https://github.com/KytyPS5/KytyPS5/pull/795) |  | DenizSAHIN570 | **P2**: tiler avoid uvec4 specialization-constant selects; take if tiler VUID/driver bug appears. | graphics: avoid uvec4 specialization-constant selects in the GPU tiler shaders |
| [#793](https://github.com/KytyPS5/KytyPS5/pull/793) |  | prompterror | **P1 when observed**: split oversized mesh draws to host limits; RED for mesh oversize. | renderer: split oversized mesh draws into host-limit slices |
| [#789](https://github.com/KytyPS5/KytyPS5/pull/789) |  | Spincial | **do not merge whole**: FNAF memory/SRT/pipeline bundle; title-led. | FNAF: Security Breach Memory Fixes. |
| [#780](https://github.com/KytyPS5/KytyPS5/pull/780) |  | itsmemac | **do not merge whole**: Spider-Man Miles Morales gameplay mega-bundle; extract only proven shared hunks. | Spider-Man: Miles Morales to gameplay: recompiler, PM4, cache, AMPR and diagnostics fixes |
| [#767](https://github.com/KytyPS5/KytyPS5/pull/767) |  | Akyy78 | **do not merge whole**: Perf/60fps experimental; scheduling/SRT mix — split after correctness. | Perf/60fps experimental |
| [#764](https://github.com/KytyPS5/KytyPS5/pull/764) |  | ElijaOwO | **separate**: HttpUriParse empty query. | Fix empty query handling in HttpUriParse |
| [#761](https://github.com/KytyPS5/KytyPS5/pull/761) |  | carbonimax | **P1 selective**: exclude fragment helpers from ballots; shared FS correctness — needs RED. | shader: exclude fragment helpers from ballots |
| [#756](https://github.com/KytyPS5/KytyPS5/pull/756) |  | Ofacy | **separate**: GitHub issue template label. | Add appropriate label to Game Emulation Status Report template |
| [#751](https://github.com/KytyPS5/KytyPS5/pull/751) |  | rudy-07 | **P2/separate**: Windows PEEK/WAITALL (peer of #797/#508). | net: resolve guest PEEK and WAITALL flags on Windows |
| [#749](https://github.com/KytyPS5/KytyPS5/pull/749) |  | herrMirto | **P2**: bound SDL event waits for main-thread tasks; WSI responsiveness. | Bound SDL event waits to keep main-thread tasks responsive |
| [#748](https://github.com/KytyPS5/KytyPS5/pull/748) |  | TYFALY | **separate**: macOS ARM64/Tracy/CMake. | Fix macOS ARM64, Tracy integration, and CMake improvements |
| [#747](https://github.com/KytyPS5/KytyPS5/pull/747) |  | vanshrana369 | **do not merge whole**: Astrobot raytracing. | Astrobot raytracing |
| [#741](https://github.com/KytyPS5/KytyPS5/pull/741) |  | carbonimax | **P2**: prune unused mesh outputs. | graphics: prune unused mesh outputs |
| [#740](https://github.com/KytyPS5/KytyPS5/pull/740) |  | Kody-Schram | **separate**: Linux file locations. | Fix Linux file locations |
| [#735](https://github.com/KytyPS5/KytyPS5/pull/735) |  | carbonimax | **P2 performance**: eliminate uniform read-first-lane; profile first. | shader: eliminate uniform read-first-lane operations |
| [#732](https://github.com/KytyPS5/KytyPS5/pull/732) |  | anatoliikuc | **separate**: auto-hide cursor. | auto-hide cursor on idle and start (window) |
| [#727](https://github.com/KytyPS5/KytyPS5/pull/727) |  | carbonimax | **P2**: wave32 NGG passthrough; not current wave64 Yōtei path. | Support wave32 NGG passthrough programs |
| [#724](https://github.com/KytyPS5/KytyPS5/pull/724) |  | Aspenini | **separate**: Zarchive loading. | Zarchive Loading |
| [#721](https://github.com/KytyPS5/KytyPS5/pull/721) |  | carbonimax | **separate**: MoltenVK BDA buffer lifetime. | vulkan: keep MoltenVK device-address buffers alive through queued work |
| [#720](https://github.com/KytyPS5/KytyPS5/pull/720) |  | nomolao2-cell | **do not merge whole**: Astro Bot Intel CPU compat. | Astro Bot Intel CPU compatibility fixes |
| [#719](https://github.com/KytyPS5/KytyPS5/pull/719) |  | Ekt0re | **diagnostic**: async logger + crash handler. | feat(logging): add asynchronous logger and crash handler |
| [#718](https://github.com/KytyPS5/KytyPS5/pull/718) |  | FirasDev | **P1 later / not now**: precompile recorded set — warm 2nd+ boot only; cold unchanged; keyed on `KYTY_GIT_REVISION`; CONFLICTING vs tip (`CMakeLists`/`pipelineCache`/`window`). Does not cut mid-run 54904 GPUAV CreatePipeline (~290 s×N). Needs identity RED before port. | graphics: precompile the recorded shader set before the title runs |
| [#717](https://github.com/KytyPS5/KytyPS5/pull/717) |  | YuhiAida | **P2**: clamp host mip levels / layered block views. | graphics: clamp host mip levels and gate layered block views |
| [#715](https://github.com/KytyPS5/KytyPS5/pull/715) |  | carbonimax | **do not merge whole**: native GPU PerVertex replay + shader caches prototype. | graphics: prototype native GPU PerVertex replay and shader caches |
| [#709](https://github.com/KytyPS5/KytyPS5/pull/709) |  | Ekt0re | **P2**: zero-sized/minimized surface crash. | Fix: Prevent crash when window has zero-sized/minimized surface |
| [#706](https://github.com/KytyPS5/KytyPS5/pull/706) |  | YuhiAida | **P2**: emulate sRGB decode for narrow formats (#415 class). | shader: emulate the sRGB decode of narrow sRGB sampled formats |
| [#702](https://github.com/KytyPS5/KytyPS5/pull/702) |  | MehmetCambaz | **P2 performance**: avoid unnecessary GPU drains for bool predicates. | graphics: avoid unnecessary GPU drains for bool predicates |
| [#693](https://github.com/KytyPS5/KytyPS5/pull/693) |  | 1OO1O11O | **P2**: 64-bit COPY_DATA immediates + indexed event queue. | Preserve 64-bit COPY_DATA immediates and add indexed event queue lookup |
| [#691](https://github.com/KytyPS5/KytyPS5/pull/691) |  | 1OO1O11O | **separate**: IME dialog validation. | Fix IME dialog validation and redraw tracking |
| [#690](https://github.com/KytyPS5/KytyPS5/pull/690) |  | bipinkrish | **P1 selective**: dynamic buffer descriptors + memory ops; needs RED, do not merge tests blindly. | add dynamic buffer descriptor support, enhance memory operations, and add compute tests |
| [#685](https://github.com/KytyPS5/KytyPS5/pull/685) |  | carbonimax | **separate**: MoltenVK descriptor sets. | vulkan: use descriptor sets for MoltenVK shader pipelines |
| [#666](https://github.com/KytyPS5/KytyPS5/pull/666) |  | c0sxm0s | **separate**: launcher update feed. | launcher: keep primary update feed answer when the fallback request f… |
| [#654](https://github.com/KytyPS5/KytyPS5/pull/654) |  | shadowbeat070 | **do not merge whole**: Silent Hill bringup. | Silent hill bringup |
| [#643](https://github.com/KytyPS5/KytyPS5/pull/643) |  | carbonimax | **P2**: centroid/vertex-pixel interpolation. | shader: align centroid and vertex-pixel interpolation |
| [#640](https://github.com/KytyPS5/KytyPS5/pull/640) |  | carbonimax | **do not merge whole / RT**: software BVH MIMG 0xe6; not Yōtei menu path. | shader: implement software BVH ray intersection for MIMG 0xe6 |
| [#638](https://github.com/KytyPS5/KytyPS5/pull/638) |  | bipinkrish | **P1 when observed**: on-demand GC + fallback downloads under GPU memory pressure. | handle GPU memory pressure with on-demand GC and fallback downloads |
| [#637](https://github.com/KytyPS5/KytyPS5/pull/637) |  | carbonimax | **P2**: fold constant lane-mask bit tests. | shader: fold constant lane-mask bit tests |
| [#633](https://github.com/KytyPS5/KytyPS5/pull/633) |  | carbonimax | **separate**: macOS startup without optional Vulkan features. | macOS: allow startup without optional Vulkan shader features |
| [#628](https://github.com/KytyPS5/KytyPS5/pull/628) |  | MehmetCambaz | **P1 performance**: SRT evaluator memo by dense plan slots (#484 class). | graphics: index the SRT evaluator memo by dense plan slots |
| [#618](https://github.com/KytyPS5/KytyPS5/pull/618) |  | LordixDemon | **P2/separate**: Windows futex via WaitOnAddress. | kernel: implement native Windows futex via WaitOnAddress |
| [#616](https://github.com/KytyPS5/KytyPS5/pull/616) |  | bipinkrish | **P2/separate**: PRX symbol resolution / user ID alias. | fix: PRX symbol resolution and user ID aliasing |
| [#613](https://github.com/KytyPS5/KytyPS5/pull/613) | yes | psnwd | **draft/do not merge whole**: Beast of Reincarnation graphics shader fixes. | Graphics shader fixes (Game: Beast of Reincarnation) |
| [#607](https://github.com/KytyPS5/KytyPS5/pull/607) |  | psnwd | **separate**: PSVR2 stubs. | vr: stub Hmd2/VrTracker2, opt-in PSVR2 video out |
| [#603](https://github.com/KytyPS5/KytyPS5/pull/603) |  | Absolute93 | **test P1**: CI CPU regressions on hosted runners. | ci: run CPU regressions and cache compilation on hosted runners |
| [#602](https://github.com/KytyPS5/KytyPS5/pull/602) | yes | Ekt0re | **draft/separate**: DualSense via keyboard. | Translation of DualSense commands using keyboard keys |
| [#599](https://github.com/KytyPS5/KytyPS5/pull/599) | yes | chenxiao07 | **draft/do not merge whole**: AI Demons Souls experiments reference. | Reference: AI-assisted Demon's Souls CPU and rendering experiments |
| [#565](https://github.com/KytyPS5/KytyPS5/pull/565) |  | psnwd | **test**: kernel filesystem test on Windows. | tests: link the kernel filesystem test on Windows |
| [#563](https://github.com/KytyPS5/KytyPS5/pull/563) |  | psnwd | **diagnostic P2**: module+cause for guest fault (peer #836). | loader: report the module and cause behind a guest fault |
| [#562](https://github.com/KytyPS5/KytyPS5/pull/562) |  | MehmetCambaz | **P2**: BDA upload from CPU-dirty hints. | graphics: find BDA upload work from CPU-dirty hints |
| [#558](https://github.com/KytyPS5/KytyPS5/pull/558) |  | defektu | **do not merge whole**: Astro Bot RT + SRT plan bug. | Astro Bot: ray tracing, and the SRT plan bug that dropped its lighting |
| [#555](https://github.com/KytyPS5/KytyPS5/pull/555) |  | FirasDev | **do not merge whole**: BVH resource classification / RT crash. | shader: fix BVH resource classification and specialization crash |
| [#553](https://github.com/KytyPS5/KytyPS5/pull/553) |  | psnwd | **separate**: launcher input icons. | launcher: show controller and key icons in the input mapping dialog |
| [#537](https://github.com/KytyPS5/KytyPS5/pull/537) |  | CheesyPoofs346 | **P1 when observed**: publish linear storage images before CPU access; black/stale texture class. | renderer: publish linear storage images before CPU access |
| [#536](https://github.com/KytyPS5/KytyPS5/pull/536) |  | SaifMalik0162 | **separate**: macOS aggregate test build. | Fix macOS aggregate test build |
| [#532](https://github.com/KytyPS5/KytyPS5/pull/532) | yes | CheesyPoofs346 | **draft/P1 selective**: finalize bindings after range-changing ops. | renderer: finalize bindings after every range-changing operation |
| [#512](https://github.com/KytyPS5/KytyPS5/pull/512) |  | Almo7aya | **do not merge whole**: Uncharted 4 compat mega-bundle. | Improve Uncharted 4 compat shaders, GPU execution, media playback |
| [#508](https://github.com/KytyPS5/KytyPS5/pull/508) |  | FirasDev | **P2/separate**: Windows PEEK+WAITALL | net: drop MSG_WAITALL when combined with MSG_PEEK on Windows |
| [#506](https://github.com/KytyPS5/KytyPS5/pull/506) |  | Techx3 | **P1 perf split first**: GPU scheduling/cache FPS | renderer: reduce FPS overhead in GPU scheduling and resource caching |
| [#497](https://github.com/KytyPS5/KytyPS5/pull/497) | yes | fxpw | **current integration**: yotei-windows-bringup draft; spinner proven; menu PENDING. | WIP: advance Ghost of Yōtei bring-up on Windows |
| [#490](https://github.com/KytyPS5/KytyPS5/pull/490) |  | francoisatt | **see 8.09 catalog**: historical row below if still listed | shader: IMAGE_ATOMIC_FMIN/FMAX (MIMG 0x1e/0x1f) |
| [#484](https://github.com/KytyPS5/KytyPS5/pull/484) |  | Leclowndu93150 | **see 8.09 catalog**: historical row below if still listed | shader: reuse SRT evaluator scratch between draws |
| [#483](https://github.com/KytyPS5/KytyPS5/pull/483) |  | Leclowndu93150 | **see 8.09 catalog**: historical row below if still listed | renderer: skip GPU sync when unmapping non-GPU memory |
| [#476](https://github.com/KytyPS5/KytyPS5/pull/476) |  | MehmetCambaz | **see 8.09 catalog**: historical row below if still listed | renderer: preserve byte offsets for sub-dword storage buffers |
| [#473](https://github.com/KytyPS5/KytyPS5/pull/473) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | shader: query host readability once per region during the SRT walk |
| [#463](https://github.com/KytyPS5/KytyPS5/pull/463) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | shader: wave32 lane-mask and compare writes leave vcc_hi and exec_hi untouched |
| [#462](https://github.com/KytyPS5/KytyPS5/pull/462) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | shader: storage image writes skip out-of-range texels |
| [#461](https://github.com/KytyPS5/KytyPS5/pull/461) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | cfg: sweep post-dominators in reverse block order |
| [#459](https://github.com/KytyPS5/KytyPS5/pull/459) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | shader: do not dereference an unresolved descriptor address |
| [#458](https://github.com/KytyPS5/KytyPS5/pull/458) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | shader: support the remaining wave32 SAVEEXEC variants |
| [#448](https://github.com/KytyPS5/KytyPS5/pull/448) |  | MehmetCambaz | **see 8.09 catalog**: historical row below if still listed | shader: apply a storage buffer's host offset at byte granularity |
| [#443](https://github.com/KytyPS5/KytyPS5/pull/443) |  | MehmetCambaz | **see 8.09 catalog**: historical row below if still listed | renderer: do not treat leftover depth-view write disables as a bound … |
| [#431](https://github.com/KytyPS5/KytyPS5/pull/431) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | graphics: resolve table border colors from the guest border color table |
| [#429](https://github.com/KytyPS5/KytyPS5/pull/429) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | graphics: materialize CMask fast clears and decode wide clear values |
| [#420](https://github.com/KytyPS5/KytyPS5/pull/420) |  | MehmetCambaz | **see 8.09 catalog**: historical row below if still listed | shader: cut the allocations done for every draw call |
| [#409](https://github.com/KytyPS5/KytyPS5/pull/409) |  | MehmetCambaz | **see 8.09 catalog**: historical row below if still listed | common: report crashes that reach no handler |
| [#403](https://github.com/KytyPS5/KytyPS5/pull/403) |  | brandostrong | **see 8.09 catalog**: historical row below if still listed | shader: return zero from the clamp output modifier for NaN |
| [#402](https://github.com/KytyPS5/KytyPS5/pull/402) |  | MehmetCambaz | **see 8.09 catalog**: historical row below if still listed | libs: make the per-library tracing flag reach the traced code |
| [#379](https://github.com/KytyPS5/KytyPS5/pull/379) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | loader: validate ELF/SELF bounds with aligned-tail compatibility |
| [#377](https://github.com/KytyPS5/KytyPS5/pull/377) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | tests: cover 1D-array color render targets |
| [#375](https://github.com/KytyPS5/KytyPS5/pull/375) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | renderer: clamp render area to attachment mip views |
| [#374](https://github.com/KytyPS5/KytyPS5/pull/374) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | renderer: clamp image copies to shared extents |
| [#373](https://github.com/KytyPS5/KytyPS5/pull/373) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | renderer: preserve GPU-authored storage images during GC |
| [#369](https://github.com/KytyPS5/KytyPS5/pull/369) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | shader: honor FP16 overflow mode across shader stages |
| [#367](https://github.com/KytyPS5/KytyPS5/pull/367) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | shader: honor DX10_CLAMP NaN mode across shader stages |
| [#362](https://github.com/KytyPS5/KytyPS5/pull/362) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | shader: make FP16 round-to-even deterministic |
| [#353](https://github.com/KytyPS5/KytyPS5/pull/353) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | shader: ignore unreachable descriptor Phi inputs |
| [#344](https://github.com/KytyPS5/KytyPS5/pull/344) |  | Techx3 | **see 8.09 catalog**: historical row below if still listed | kernel: implement POSIX file metadata APIs |
| [#340](https://github.com/KytyPS5/KytyPS5/pull/340) |  | foufouadi | **see 8.09 catalog**: historical row below if still listed | shader: implement Yotei boot shader blockers |
| [#338](https://github.com/KytyPS5/KytyPS5/pull/338) |  | foufouadi | **see 8.09 catalog**: historical row below if still listed | shader: IMAGE_ATOMIC_FMIN/FMAX + DPP8 (Ghost of Yōtei boot) |
| [#322](https://github.com/KytyPS5/KytyPS5/pull/322) |  | chaitan3 | **see 8.09 catalog**: historical row below if still listed | Fix crash dialog when running the emulator under wine on linux |
| [#247](https://github.com/KytyPS5/KytyPS5/pull/247) |  | Hashim1999164 | **see 8.09 catalog**: historical row below if still listed | logging: rotate printf log files to cap report size |
| [#241](https://github.com/KytyPS5/KytyPS5/pull/241) |  | Pouare514 | **see 8.09 catalog**: historical row below if still listed | fix: SAMPLE_C color sampling, GPU write-watch, and Windows CET guest stacks |
| [#167](https://github.com/KytyPS5/KytyPS5/pull/167) |  | Death-Whispers-U | **see 8.09 catalog**: historical row below if still listed | Decode sRGB in the shader for formats the host image cannot carry |
| [#148](https://github.com/KytyPS5/KytyPS5/pull/148) |  | Stepz97 | **see 8.09 catalog**: historical row below if still listed | macOS boot hang after "Relocate program": ProtectGuestMemory applies guest-only protection to the host mapping (regression from #135) |


---

## Исторический детальный каталог (снимок 8.09; PR ≤509)

Обозначения: **P1** — рассмотреть скоро отдельным regression-first изменением;
**P2** — полезный общий механизм, но нет связи с текущим симптомом; **covered** —
эквивалент уже есть; **diagnostic/test** — улучшает поиск ошибок; **separate** —
другая платформа/подсистема; **do not merge whole** — извлекать только доказанные
части.

| PR | Оценка для текущей ветки |
| --- | --- |
| [#42 DT_REL loader](https://github.com/KytyPS5/KytyPS5/pull/42) | **separate:** полезно для legacy/homebrew ELF, но retail Yōtei уже проходит loader и использует другой relocation path. |
| [#148 macOS ProtectGuestMemory](https://github.com/KytyPS5/KytyPS5/pull/148) | **separate:** macOS-only boot regression; к native Windows запуску отношения нет. |
| [#167 shader sRGB decode](https://github.com/KytyPS5/KytyPS5/pull/167) | **P2:** правильный класс для `R8/R8G8 sRGB`; предпочесть более новый и узкий #415, если capture подтвердит такие форматы. |
| [#215 safe-point guest signals](https://github.com/KytyPS5/KytyPS5/pull/215) | **do not merge whole:** потенциально лечит Unity deadlocks, но автор сам не заявляет точную семантику; нужен отдельный scheduler/signal RED. |
| [#241 SAMPLE_C/write-watch/CET](https://github.com/KytyPS5/KytyPS5/pull/241) | **do not merge whole:** старый широкий набор. SAMPLE_C, cache invalidation и CET stack fixes разбирать независимо по фактическому сбою. |
| [#247 printf log rotation](https://github.com/KytyPS5/KytyPS5/pull/247) | **diagnostic:** ограничивает размер printf logs; полезно для диска, но может удалить ранний контекст длинного прогона. |
| [#322 Wine crash dialog](https://github.com/KytyPS5/KytyPS5/pull/322) | **separate:** только Windows build под Wine, не native Windows. |
| [#338 Yōtei atomics + DPP8](https://github.com/KytyPS5/KytyPS5/pull/338) | **covered:** текущая ветка содержит более полную реализацию и тесты. |
| [#340 Yōtei boot blockers](https://github.com/KytyPS5/KytyPS5/pull/340) | **covered:** дубликат старого boot-набора #338. |
| [#344 POSIX metadata APIs](https://github.com/KytyPS5/KytyPS5/pull/344) | **separate/P2:** полезная kernel completeness, но текущий Yōtei не остановлен на `fchmod/futimes/utimes`. |
| [#352 streaming ATRAC9 RIFF](https://github.com/KytyPS5/KytyPS5/pull/352) | **separate/P2:** может помочь audio streaming позже, не shader/render blocker. |
| [#353 unreachable descriptor Phi](https://github.com/KytyPS5/KytyPS5/pull/353) | **do not merge as-is:** идея полезна, но proof недостаточен; текущая ветка имеет более осторожный uniform descriptor Phi lowering. |
| [#361 logical lane IDs](https://github.com/KytyPS5/KytyPS5/pull/361) | **covered:** intent включён в более полную split-wave64 модель. |
| [#362 deterministic FP16 RNE](https://github.com/KytyPS5/KytyPS5/pull/362) | **P2:** хорошая cross-driver shader correctness; брать после capture/RED или отдельным conformance циклом. |
| [#367 DX10_CLAMP NaN mode](https://github.com/KytyPS5/KytyPS5/pull/367) | **P2:** register bits уже декодируются, но полная semantics/cache identity полезна; прямой связи с чёрным кадром нет. |
| [#369 FP16 overflow mode](https://github.com/KytyPS5/KytyPS5/pull/369) | **P2:** аналогично #367; полезная точность, но не текущий доказанный blocker. |
| [#373 preserve GPU-authored storage images](https://github.com/KytyPS5/KytyPS5/pull/373) | **P1:** сильный общий anti-black-texture кандидат; адаптировать с GC progress/readback RED. |
| [#374 clamp image copies](https://github.com/KytyPS5/KytyPS5/pull/374) | **P2:** предотвращает copy-overrun/VUID на alias extents; брать при соответствующем validation error. |
| [#375 clamp render area](https://github.com/KytyPS5/KytyPS5/pull/375) | **P2:** предотвращает renderArea > mip view; текущих VUID этого класса нет. |
| [#376 BC storage aliases](https://github.com/KytyPS5/KytyPS5/pull/376) | **P2:** полезно для compressed storage aliases, но текущий целевой surface не BC. |
| [#377 1D-array RT tests](https://github.com/KytyPS5/KytyPS5/pull/377) | **test:** production не меняет; хороший соседний regression test. |
| [#379 ELF/SELF bounds](https://github.com/KytyPS5/KytyPS5/pull/379) | **P2/separate:** полезная loader hardening и retail aligned-tail compatibility; текущие модули уже загружаются. |
| [#383 heterogeneous indirect images](https://github.com/KytyPS5/KytyPS5/pull/383) | **closed unmerged (25.09) / covered selectively:** достигнутые sampled dimension/swizzle и storage write specialization подклассы уже на ветке; не воскрешать PR. |
| [#402 library tracing flag](https://github.com/KytyPS5/KytyPS5/pull/402) | **diagnostic:** полезен для массовой диагностики library calls, без изменения guest behavior. |
| [#403 NaN saturate](https://github.com/KytyPS5/KytyPS5/pull/403) | **P2:** корректная явная NaN semantics; пересекается с #430, выбрать один минимальный вариант после RED. |
| [#409 unhandled crash reporting](https://github.com/KytyPS5/KytyPS5/pull/409) | **diagnostic P1:** ценно для длинных запусков и silent termination; проверить reentrancy/flush отдельно. |
| [#414 `_umtx_op`](https://github.com/KytyPS5/KytyPS5/pull/414) | **P2/separate:** крупная missing kernel primitive для других игр; Yōtei уже выполняет многопоточный runtime. |
| [#415 narrow sRGB sampling](https://github.com/KytyPS5/KytyPS5/pull/415) | **P2:** предпочтительный кандидат вместо #167 при подтверждённых `k8Srgb/k8_8Srgb` descriptors. |
| [#420 per-draw allocation cuts](https://github.com/KytyPS5/KytyPS5/pull/420) | **P1 performance:** материализация вызывается часто; измерить на текущем профиле и проверить lifetime/invalidation pool. |
| [#427 Spider-Man bundle](https://github.com/KytyPS5/KytyPS5/pull/427) | **do not merge whole:** отдельные opcode/build fixes возможны, но skip-unsupported и fixed wave scratch неприемлемы как общее решение. |
| [#429 CMask fast clears](https://github.com/KytyPS5/KytyPS5/pull/429) | **P1 when observed:** важная renderer semantics; текущий Yōtei target trace явно не использует CMask. |
| [#430 NaN clamp](https://github.com/KytyPS5/KytyPS5/pull/430) | **P2/duplicate class:** пересекается с #403; не брать оба. |
| [#431 sampler border table](https://github.com/KytyPS5/KytyPS5/pull/431) | **P2:** может исправить края/цвет текстур при table border mode; нет текущего evidence. |
| [#432 matching fixed map](https://github.com/KytyPS5/KytyPS5/pull/432) | **P2/separate:** полезная idempotent memory mapping semantics для соответствующего guest pattern. |
| [#434 SaveData metadata](https://github.com/KytyPS5/KytyPS5/pull/434) | **separate:** важна для сохранений после достижения gameplay, не для bring-up кадра. |
| [#439 TLS patch boundaries](https://github.com/KytyPS5/KytyPS5/pull/439) | **P2:** loader safety against patching data as instructions; брать только с malformed/boundary RED. |
| [#443 stale depth write-disable bits](https://github.com/KytyPS5/KytyPS5/pull/443) | **P2:** маленький корректный-looking unbound-depth fix; нынешний Yōtei не падает с этим fatal. |
| [#446 Hi-Stencil test fixture](https://github.com/KytyPS5/KytyPS5/pull/446) | **covered:** текущая fixture уже задаёт требуемый флаг явно. |
| [#447 recompiler tests in CI](https://github.com/KytyPS5/KytyPS5/pull/447) | **test P1:** полезно против массовых regressions; CPU-only scope корректен для hosted CI. |
| [#448 byte-granularity storage offset](https://github.com/KytyPS5/KytyPS5/pull/448) | **covered/superseded:** старее #476 и уже адаптирован в текущей ветке. |
| [#450 BDA/image ownership](https://github.com/KytyPS5/KytyPS5/pull/450) | **do not merge as-is:** идеи полезны, но pointer traversal и owner/readback contract требуют переработки. |
| [#457 image resource limit 64](https://github.com/KytyPS5/KytyPS5/pull/457) | **covered stronger:** текущий cap 512 плюс actual Vulkan limit guards. |
| [#458 remaining SAVEEXEC_B32](https://github.com/KytyPS5/KytyPS5/pull/458) | **P1 decode coverage:** восемь форм отсутствуют; пока corpus не показал конкретный failure, поэтому после #468. |
| [#459 unresolved descriptor read](https://github.com/KytyPS5/KytyPS5/pull/459) | **covered:** адаптировано и проверено в `1224cc8`. |
| [#460 SRT visiting cleanup](https://github.com/KytyPS5/KytyPS5/pull/460) | **covered/latent:** cleanup уже есть; отдельного game effect текущий API не доказывает. |
| [#461 reverse post-dominator sweep](https://github.com/KytyPS5/KytyPS5/pull/461) | **P1 performance:** текущий код всё ещё идёт forward; маленькое изменение может резко сократить CFG compile time. |
| [#462 OOB storage image writes](https://github.com/KytyPS5/KytyPS5/pull/462) | **P1/P2 correctness:** предотвращает undefined Vulkan access; нужен exact guest/Vulkan semantics RED. |
| [#463 wave32 high mask words](https://github.com/KytyPS5/KytyPS5/pull/463) | **P1:** потенциальный device-loss fix; проверить точный write footprint до переноса. |
| [#464 vertex buffer descriptor size](https://github.com/KytyPS5/KytyPS5/pull/464) | **P1:** небольшой общий bounds fix, полезный против мусорных vertex attributes. |
| [#465 removed ES/LS registers](https://github.com/KytyPS5/KytyPS5/pull/465) | **P2:** общий GFX10 register-hole fix, подтверждён другим title; не текущий Yōtei blocker. |
| [#467 SSE4a EXTRQ/INSERTQ](https://github.com/KytyPS5/KytyPS5/pull/467) | **P2/separate:** полезная CPU/loader compatibility, если guest реально исполняет register forms. |
| [#468 `S_WQM_B32`](https://github.com/KytyPS5/KytyPS5/pull/468) | **closed unmerged (25.09):** numeric WQM на ветке шире старой boolean-модели; при новом decode gap — свой RED, не revive PR. |
| [#470 EXEC/VCC ballots](https://github.com/KytyPS5/KytyPS5/pull/470) | **covered/draft:** текущая numeric split-wave64 модель шире; не накладывать старую ветку. |
| [#473 SRT readable-region cache](https://github.com/KytyPS5/KytyPS5/pull/473) | **P2 performance:** ускоряет raw fallback; основной game path использует bounded callbacks, поэтому сначала профиль. |
| [#476 sub-DWORD storage offsets](https://github.com/KytyPS5/KytyPS5/pull/476) | **covered/adapted:** текущая версия уже строже по admission и имеет overflow/atomic guards. |
| [#477 Demon's Souls bundle](https://github.com/KytyPS5/KytyPS5/pull/477) | **do not merge whole:** слишком широкий и содержит недоказанную descriptor harmonization. |
| [#483 skip GPU sync on non-GPU unmap](https://github.com/KytyPS5/KytyPS5/pull/483) | **P1 performance:** маленький кандидат для streaming workloads; нужен race/map-range RED и профиль Yōtei. |
| [#484 reuse SRT evaluator scratch](https://github.com/KytyPS5/KytyPS5/pull/484) | **P1 performance:** текущий evaluator ещё резервирует scratch на materialization; проверить generation/lifetime contract. |
| [#488 minimized window crash](https://github.com/KytyPS5/KytyPS5/pull/488) | **P2/separate:** полезный WSI fix, но не влияет на обычный не-minimized запуск. |
| [#490 float image atomics](https://github.com/KytyPS5/KytyPS5/pull/490) | **covered:** production semantics уже в ветке; текущий PR head лишь другая актуализация того же класса. |
| [#493 opt-in BVH stub](https://github.com/KytyPS5/KytyPS5/pull/493) | **diagnostic only:** decode/message полезны; always-miss не является реализацией ray tracing и не нужен Yōtei сейчас. |
| [#497 current Yōtei draft](https://github.com/KytyPS5/KytyPS5/pull/497) | **current integration (26.09):** MERGEABLE vs main `fd2e15ee`; spinner proven (`…-081515`); long-run watchdog shown=159 on 54904 (`…-081840`); **no foreign PR merged** this pass; menu PENDING. |
| [#500 Demon's Souls shader work](https://github.com/KytyPS5/KytyPS5/pull/500) | **closed unmerged (25.09) / partly covered:** часть уже перенесена; остаток только по отдельному RED, не revive whole. |
| [#503 negative printf precision](https://github.com/KytyPS5/KytyPS5/pull/503) | **P2/separate:** корректный libc fix с хорошим focused coverage; не связан с renderer/shader failure. |
| [#504 rejected-open descriptor leak](https://github.com/KytyPS5/KytyPS5/pull/504) | **P2/separate:** однострочный kernel cleanup с тестами; полезен глобально, не текущему run. |
| [#506 GPU scheduling/cache FPS](https://github.com/KytyPS5/KytyPS5/pull/506) | **P1 performance after correctness, split first:** четыре полезных идеи, но они не устраняют измеренный shader-driver stall; разные correctness/race contracts, не cherry-pick одним блоком. |
| [#508 Windows PEEK+WAITALL](https://github.com/KytyPS5/KytyPS5/pull/508) | **P2/separate:** чинит Windows test/socket compatibility; workaround не является полной WAITALL emulation. |
| [#509 primitive restart/watcher/vertex](https://github.com/KytyPS5/KytyPS5/pull/509) | **P1 selective/do not merge whole:** custom restart и watcher lifecycle полезны; fallback invalid selectors в ноль неприемлем без доказательства. |

## Проверки переснимка 26.09

- GitHub REST: `GET /repos/KytyPS5/KytyPS5/pulls?state=open&per_page=100` paginated → **113** PR.
- Inventory TSV: `_Build` local `/tmp/open-prs-20260926.tsv` (not committed); numbers 148–845.
- Changed-file sets fetched for interest set #497/#506/#509/#512/#532/#537/#555/#558/#562/#618/#628/#638/#640/#685/#690/#702/#706/#715/#717/#718/#727/#735/#741/#761/#767/#780/#789/#793/#795/#797/#799/#808/#811/#818/#820/#827/#833–#845.
- No full checkout/build of all 113 heads; triage is metadata+paths+Yōtei runtime context.
- Author game claims are hints, not our verification.
