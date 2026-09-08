Дата обзора: **8 сентября 2026 года**

# Открытые PR KytyPS5: что полезно для текущего bring-up

Снимок GitHub: **68 открытых PR** на 8 сентября 2026 года. Два из них draft:
[#470](https://github.com/KytyPS5/KytyPS5/pull/470) и
[#497](https://github.com/KytyPS5/KytyPS5/pull/497). Сравнение выполнено с веткой
`yotei-windows-bringup`: первоначальный обзор — на `b247c0f`, runtime-уточнение
ниже — после `90fed2b` (`shader: prove signed descriptor loops in dispatcher CFG`).

Проверены метаданные, описания и состав diff всех 68 PR. Для shader/renderer,
новых PR #493/#497/#500/#503/#504/#506/#508/#509 и кандидатов, пересекающихся
с текущим запуском, дополнительно просмотрены изменения кода и текущее состояние
ветки. Это triage и адресный code review, а не утверждение, что все 68 веток
собраны и прогнаны. Авторские игровые результаты считаются подсказкой, а не нашей
верификацией.

## Короткий вывод

Готового PR, который можно целиком влить и тем самым получить первый ненулевой
кадр Ghost of Yōtei, **нет**. Run на `b247c0f`, дошедший до frame 179, не
компилировал проблемный вариант `6cc64dee…`, поэтому прежний вывод о закрытом PC
`0x656c` был преждевременным. Следующий run воспроизвёл отказ: точный шейдер
использует signed runtime loop, stride 196 и четыре correlated descriptor words,
а не constant guarded selector из регрессии `b247c0f`.

Нужный общий bounded-read proof уже существовал, но был безусловно отключён для
dispatcher CFG. В `90fed2b` dispatcher теперь строит и проверяет полный CFG и
использует тот же строгий proof. Неизменённый synthetic RED, точный manifest и
игра проходят PC `0x656c`. Новый первый fatal — `indirect image table` с
несовместимыми candidates на PC `0x7c4`; поэтому ближайший доказанный кандидат —
selective port общего механизма heterogeneous indirect images из **#383**.

Самые полезные следующие кандидаты:

1. **#383 — heterogeneous indirect images.** Новый bounded run упирается именно
   в несовместимые candidates indirect image table на PC `0x7c4`. Сначала нужен
   точный RED по найденному shader manifest, затем перенос только общей модели
   heterogeneous candidates с сохранением текущих null/immutable/limit checks.
2. **#468 — `S_WQM_B32`.** Это подтверждённый decode gap двух shader manifests.
   Нужен адаптированный numeric/raw-word вариант с RED/GREEN, а не прямой перенос
   старой boolean-модели.
3. **#373 — сохранение GPU-authored storage images при GC.** Это реальный общий
   класс потери единственной актуальной копии изображения и потенциальная причина
   чёрных/испорченных текстур. Он не доказан причиной текущего Yōtei-кадра, но
   correctness-защита важнее одной лишь отсрочки GC из #506.
4. **#463 — сохранение `vcc_hi`/`exec_hi` в wave32.** Может устранить shader loop
   hang/device loss; сначала нужен точный sentinel RED по ISA, потому что старый
   обзор не принял утверждение о footprint записи без проверки.
5. **#429 — CMask fast clears.** Общая недостающая renderer-семантика, способная
   оставлять цветовые поверхности пустыми. В последнем изученном Yōtei trace у
   целевого пути `cmask_fast_clear_enable=false` и `cmask=0`, поэтому это не
   объяснение текущего кадра; брать при первом фактическом CMask producer.
6. **#462 и #464 — OOB image stores и точный размер vertex buffer.** Небольшие
   общие correctness-кандидаты, которые предотвращают undefined Vulkan access и
   мусорные vertex attributes. Нужны отдельные регрессии.
7. **#420/#461/#483/#484/#506 — производительность.** Они не создадут правильный
   кадр, но могут существенно ускорить длинный поиск последовательных блокеров.
   Сначала профилировать текущую ветку; #506 разделить как минимум на четыре
   независимых изменения (RELEASE_MEM batching, GC threshold, read-only barrier,
   block descriptor reads).
8. **#509 — только отдельные части.** Custom primitive-restart remap и устранение
   write-watcher accumulation заслуживают самостоятельных RED. Весь PR переносить
   нельзя: fallback reserved/invalid vertex selectors в ноль скрывает unsupported
   guest semantics, а большая несвязанная переработка descriptors не имеет
   изолированных тестов на нашей ветке.
9. **#409/#402/#447 — диагностика и CI.** Не исправляют игру напрямую, но уменьшают
   немые падения, делают library tracing рабочим и запускают CPU recompiler tests
   в CI. Это полезная инфраструктура для массового поиска ошибок.

## Что уже покрыто текущей веткой

- #338/#340/#490: IMAGE_ATOMIC_FMIN/FMAX и DPP8 перенесены и расширены native
  regression-тестами; повторный merge не нужен.
- #361/#470: logical lane ID и numeric EXEC/VCC/ballot покрыты более широкой
  split-wave64 моделью. Draft #470 накладывать нельзя.
- #457: локальный предел уже заменён `MaxImages=512` и проверками реальных Vulkan
  layout/device limits; слепое `32 -> 64` слабее текущего решения.
- #459: unreadable raw SRT fallback закрыт коммитом `1224cc8`.
- #460: текущий `Evaluator` уже делает `m_visiting.pop_back()` на failure; заявленный
  latent cleanup присутствует.
- Signed runtime-loop proof для scalar-buffer descriptor tables уже был в
  `BoundedReadProof`; `90fed2b` применяет его к dispatcher только после построения
  и полной проверки guest CFG. Отдельный перенос этой части из другого PR не нужен.
- #448/#476: byte/sub-DWORD storage offset адаптирован более узко и безопасно в
  `5c7661d`, с сохранением unsupported границ.
- #446: stencil discovery fixture в текущих тестах уже задаёт
  `htile_stencil_disabled=false` явно.
- #497 — это собственный draft текущей ветки. Он является местом интеграции, а не
  внешним источником ещё одного исправления.
- Из #500 уже выборочно перенесены нужные для достигнутого пути идеи: DWORD pattern
  clear/HTILE, sampled-depth layout и связанные общие механизмы. Остаток #500 надо
  рассматривать по одному commit только после соответствующего RED.

## Что не следует переносить целиком

- **#353:** pruning descriptor Phi имеет структурный контрпример с обходным путём;
  immediate-successor check недостаточен без edge-dominance proof.
- **#427:** широкий bundle содержит skip/non-fatal обходы и старую fixed-scratch
  wave64 модель, которая не решает текущую multi-wave scheduling семантику.
- **#450:** полезная идея BDA prefetch смешана с ownership/readback рисками;
  short-circuit может пропустить второй набор pointers, а image-owner separation
  требует более строгого теста.
- **#477:** большой bundle подменяет несовместимые non-null descriptor candidates
  exemplar-значениями. Это нарушает требование не фабриковать guest resources.
- **#493:** always-miss BVH stub допустим только как явно включаемая диагностика.
  Он не реализует ray tracing и не должен становиться production default.
- **#509:** полезные primitive-restart и watcher hunks отделить от fallback-to-zero
  и остальных renderer/shader изменений.

## Полный каталог 68 открытых PR

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
| [#383 heterogeneous indirect images](https://github.com/KytyPS5/KytyPS5/pull/383) | **P1:** вероятный следующий resource class для batch-аудита; интегрировать с current immutable tables/null/comparison rules. |
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
| [#468 `S_WQM_B32`](https://github.com/KytyPS5/KytyPS5/pull/468) | **P1:** лучший подтверждённый следующий shader decode candidate; нужен numeric adaptation. |
| [#470 EXEC/VCC ballots](https://github.com/KytyPS5/KytyPS5/pull/470) | **covered/draft:** текущая numeric split-wave64 модель шире; не накладывать старую ветку. |
| [#473 SRT readable-region cache](https://github.com/KytyPS5/KytyPS5/pull/473) | **P2 performance:** ускоряет raw fallback; основной game path использует bounded callbacks, поэтому сначала профиль. |
| [#476 sub-DWORD storage offsets](https://github.com/KytyPS5/KytyPS5/pull/476) | **covered/adapted:** текущая версия уже строже по admission и имеет overflow/atomic guards. |
| [#477 Demon's Souls bundle](https://github.com/KytyPS5/KytyPS5/pull/477) | **do not merge whole:** слишком широкий и содержит недоказанную descriptor harmonization. |
| [#483 skip GPU sync on non-GPU unmap](https://github.com/KytyPS5/KytyPS5/pull/483) | **P1 performance:** маленький кандидат для streaming workloads; нужен race/map-range RED и профиль Yōtei. |
| [#484 reuse SRT evaluator scratch](https://github.com/KytyPS5/KytyPS5/pull/484) | **P1 performance:** текущий evaluator ещё резервирует scratch на materialization; проверить generation/lifetime contract. |
| [#488 minimized window crash](https://github.com/KytyPS5/KytyPS5/pull/488) | **P2/separate:** полезный WSI fix, но не влияет на обычный не-minimized запуск. |
| [#490 float image atomics](https://github.com/KytyPS5/KytyPS5/pull/490) | **covered:** production semantics уже в ветке; текущий PR head лишь другая актуализация того же класса. |
| [#493 opt-in BVH stub](https://github.com/KytyPS5/KytyPS5/pull/493) | **diagnostic only:** decode/message полезны; always-miss не является реализацией ray tracing и не нужен Yōtei сейчас. |
| [#497 current Yōtei draft](https://github.com/KytyPS5/KytyPS5/pull/497) | **current integration:** наша рабочая ветка; после опубликованного head уже есть локальный `b247c0f`. |
| [#500 Demon's Souls shader work](https://github.com/KytyPS5/KytyPS5/pull/500) | **partly covered/do not merge whole:** часть уже перенесена; оставшиеся typed stores, indirect sync/dispatch, stencil upload и null handling брать только по отдельному RED. |
| [#503 negative printf precision](https://github.com/KytyPS5/KytyPS5/pull/503) | **P2/separate:** корректный libc fix с хорошим focused coverage; не связан с renderer/shader failure. |
| [#504 rejected-open descriptor leak](https://github.com/KytyPS5/KytyPS5/pull/504) | **P2/separate:** однострочный kernel cleanup с тестами; полезен глобально, не текущему run. |
| [#506 GPU scheduling/cache FPS](https://github.com/KytyPS5/KytyPS5/pull/506) | **P1 performance, split first:** четыре полезных идеи, но разные correctness/race contracts; не cherry-pick одним блоком. |
| [#508 Windows PEEK+WAITALL](https://github.com/KytyPS5/KytyPS5/pull/508) | **P2/separate:** чинит Windows test/socket compatibility; workaround не является полной WAITALL emulation. |
| [#509 primitive restart/watcher/vertex](https://github.com/KytyPS5/KytyPS5/pull/509) | **P1 selective/do not merge whole:** custom restart и watcher lifecycle полезны; fallback invalid selectors в ноль неприемлем без доказательства. |

## Рекомендуемая очередь без конфликтов со вторым агентом

1. Разобрать точные candidates нового `indirect image table` на PC `0x7c4` и
   воспроизвести несовместимость неизменённым synthetic RED.
2. Сверить #383 с текущими immutable table, null-resource, comparison и device-limit
   правилами; переносить только недостающую shared-семантику heterogeneous images.
3. После correctness frontier измерить #461, #420, #484, #483 и четыре части #506
   по отдельности. Оптимизации не объединять до измерений.
4. Для black-frame расследования сначала доказать потерю ownership/clear state.
   Если исчезает GPU-authored storage owner — #373; если capture показывает CMask
   fast clear — #429; если ни то ни другое, эти PR не применять «на удачу».
5. #509 разрезать на независимые primitive-restart, watcher-lifetime,
   storage-view validation и selector-semantics задачи. Последнюю оставить
   unsupported до появления спецификации/RED, не заменять произвольным нулём.

## Проверки этого обзора

- GitHub REST: `state=open`, `per_page=100`, получено 68 PR.
- Все 68 head refs получены как `refs/remotes/upstream/open-pr/<number>` без
  переключения рабочей ветки.
- Сверены названия, draft state, body, base/head SHA и changed-file sets.
- Первоначальный PR triage был read-only. Runtime-уточнение проверено synthetic
  RED/GREEN, exact-manifest audit и bounded native game run на `90fed2b`.
- Game run `_Build/runs/yotei-integrated-20260908-140320-cf909d` достиг frame 139,
  124 shown frames и сменил fatal с PC `0x656c` на indirect image table PC `0x7c4`.
- Последние runtime-логи использованы только для отрицательной проверки CMask:
  найдено `cmask_fast_clear_enable=false`, `cmask=0` на изученном target path.
