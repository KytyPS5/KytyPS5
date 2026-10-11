# Ghost of Yōtei: выбор базы и новый план работы

Обновлено 11 октября 2026 года после двух native запусков на RTX5060Ti.
Точный порядок возобновления, ветки, артефакты и незавершённая работа сохранены
в [плане следующей сессии](yotei-session-resume.md).

## Рекомендация

**Основной путь теперь — отдельная интеграционная ветка `yotei-integration-native`
от проверенного YOTEI-BRANCH6d041035.** Она создана в worktree
`G:\repos\KytyPS5\_Build\worktrees\yotei-fork-baseline-6d041035`.
Старая `yotei-windows-bringup` сохранена вместе с коммитом85494f56, тестами,
runtime/capture artifacts и незавершённым исправлением admission захватов.

Fork на нашем ПК показал интерактивное вступление и SETTINGS/CONTROLS после
Enter; заголовок6.2/9.1FPS. Это не benchmark, не проверка движения персонажа
и не доказательство правильности его sampler/resource substitutions.
Старая ветка завершила30-минутный диагностический запуск с чёрным экраном,
крупными SPIR-V модулями и driver cache1.24GB. Причину стоимости проверять по
одинаковым captured inputs; эти наблюдения обосновывают смену основного пути.

Следующий этап **после сохранения плана**: пользователь разрешает пакетный
сбор с диагностическими fallback, чтобы пройти дальше и собрать несколько
исходных ошибок за запуск. Сохранять полный вход и исходный сбой до fallback,
явно отмечать fallback-зависимые результаты и проверять исправления на исходных
входах. Использовать безопасные действующие альтернативы, ограничивать GPU/worker
время и не отправлять invalid SPIR-V. Затем исправлять общий механизм для группы,
проверять RED/GREEN/соседей и повторять затронутую пачку.

Сначала перенести совместимые capture/replay и batch инструменты. Старые48
непрошедших профилей переснять/проверить на новой базе: FP64 literal там уже
реализован, поэтому этот порт нельзя автоматически повторять. Полное слияние
историй и перенос всех старых production исправлений не планируются.

## Зафиксированные ревизии и результаты

| Вариант | Ревизия | Что подтверждено | Что ещё не подтверждено |
| --- | --- | --- | --- |
| Наша `yotei-windows-bringup` | Local HEAD `ab4480ff08b80b15cf3fe8f3727f07c56c2f76a6`; code/published PR497 head `3391a9fd9d9009bcc6128e9c1293d70dcd0bc11e` | Native CPU/GPU регрессии, точный capture/replay, исполнение кэша optimizer. Исправленный input selector доказан на реальном PS. | Нынешний menu/gameplay. Последний cold run завершился exit0 до PS617c после прерывания turn; причина выхода отдельно не записана. Это не успешный gameplay и не новый shader failure. |
| `peepostarexx/YOTEI-BRANCH` | `6d04103525b24559a496612ef8959a7666ce9a20` | Автор сообщает интро и открытый мир на RTX4090/7800X3D; документация указывает игру01.512 и обязательный `Readback linear images`. | Наш RTX5060Ti, точные условия чужих~7FPS и их соответствие этому HEAD. По уточнению пользователя, это чужой запуск. |
| SharpEmu PR926 | Head `95648c2b047fce1686345d09dce1a546fe0d35ee`, 426 commits/383files; open, API draft=false | Публичные изображения показывают меню и персонажа в деревне. CI build/test успешен; PR собирает merge-ref `5e809d7`. | Скриншот входа в игру относится к01.008/RTX5070 и показывает0.3FPS; он не доказывает gameplay именно текущего HEAD на нашей01.512 или приемлемую скорость. GPU-тесты могут пропускаться без устройства. |

Первичные источники: [fork guide на точном pin](https://github.com/peepostarexx/KytyPS5/blob/6d04103525b24559a496612ef8959a7666ce9a20/docs/yotei-fork.md),
[PR926](https://github.com/sharpemu/sharpemu/pull/926),
[скриншот Yōtei в деревне](https://github.com/user-attachments/assets/ee356489-e338-468c-8c28-6fe6d3d9ffbf),
[Sharp CI](https://github.com/sharpemu/sharpemu/actions/runs/38135745052),
[наш PR497](https://github.com/KytyPS5/KytyPS5/pull/497).
Зелёного CI именно head3391a9fd API не показал: 0 check-runs/contexts, а не
подтверждённый failed CI. Native проверки учитывать отдельно.

У current/fork общая база `77d772217310dc734ce53289110c9be0885a0577`.
После неё627 уникальных коммитов нашей линии и745 линии fork. Сравнение двух heads
затрагивает382 файла. Это граф истории с merge/rebase, а не количество независимых
исправлений. Полное слияние обеих линий создаст большой объём конфликтов и проверок.

## Главная разница подходов

Наш материализатор заранее читает CPU-кандидаты ресурсов: record×row, затем
строит конечную таблицу для shader. Новые proofs сокращают область, но для
texture-derived индекса нужны доступные согласованные данные, доказательство
каналов/инициализации и отсутствие writers. Объединение значений нескольких
изображений пока теряет точную связь конкретного record с конкретным row.
При GPU-produced или изменяемых данных это ограничение поддержки, а не просто
отсутствующая инструкция ISA.

Fork использует runtime bindless images: shader выбирает ключ, GPU сообщает
запросы, host разрешает соответствующие ресурсы. Его
[07f2e62a](https://github.com/peepostarexx/KytyPS5/commit/07f2e62aa4718559a060962022355a0e5066d090) и
[cf47f345](https://github.com/peepostarexx/KytyPS5/commit/cf47f345b5e29dab92001c7d68d5c4dd52634674)
прямо обрабатывают **PS617c7166f3308810**, BUFFER_LOAD→READFIRSTLANE→S_LOAD
и адрес `pointer + layer*368 + offset`. Это совпадение реального механизма
нашего отказа. Общая основа — те же Kyty IR/SPIR-V/Vulkan; смена всего frontend
для такого эксперимента не требуется.

Рабочий fork имеет существенные компромиссы в итоговом коде:

- [Неподдержанный выбранный sampler заменяется trilinear/wrap/fullLOD](https://github.com/peepostarexx/KytyPS5/blob/6d04103525b24559a496612ef8959a7666ce9a20/src/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp#L2674). Structs для bindless samplers существуют, но их heaps не заполняются. Это не полная эмуляция динамического S#.
- [Ошибка materialization возвращает пустой shader program](https://github.com/peepostarexx/KytyPS5/blob/6d04103525b24559a496612ef8959a7666ce9a20/src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp#L948), что может пропустить draw/dispatch. Удаление списка shader-hash skips не устранило этот путь.
- [Класс изображений оценивается по первым записям heap](https://github.com/peepostarexx/KytyPS5/blob/6d04103525b24559a496612ef8959a7666ce9a20/src/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp#L502); mixed integer/float и dimension fallbacks требуют отдельных проверок.
- [Некоторые texture failures допускают placeholder](https://github.com/peepostarexx/KytyPS5/blob/6d04103525b24559a496612ef8959a7666ce9a20/src/graphics/host_gpu/renderer/pipeline/descriptors.cpp#L557). Strict switches нужны для оценки того, какие ошибки скрывает видимый результат.

У Sharp также [первое обращение к отсутствующему resident descriptor даёт null-slot0](https://github.com/foufouadi/sharpemu/blob/95648c2b047fce1686345d09dce1a546fe0d35ee/src/SharpEmu.ShaderCompiler/Resources/RuntimeDescriptorTable.cs#L7)
до следующего tick; исходный draw не переигрывается в этом контракте.
А [record-aligned fallback при превышении probe budget](https://github.com/foufouadi/sharpemu/blob/95648c2b047fce1686345d09dce1a546fe0d35ee/src/SharpEmu.ShaderCompiler/Resources/ResourceMaterializer.cs#L714)
исключает некоторые достижимые U32-wrapped keys. Эти правила нельзя переносить
как точную семантику. Наш regression для реально достижимого wrapped R8-key
должен сохраниться при выборе любой базы.

## Два практических решения

| Решение | Когда выбирать | Плюсы | Цена |
| --- | --- | --- | --- |
| **A. Новая экспериментальная ветка от проверенного YOTEI-BRANCH** | Fork локально проходит menu/entry, current при тех же условиях отстаёт | Родственный C++ код; весь связанный bindless/rendering кластер уже совместно разработан. Быстрее получить воспроизводимую рабочую точку и затем улучшать её. | Требуется проверить и исправлять перечисленные fallbacks. Переносить нашу ветку целиком нельзя; часть изменений может быть уже покрыта или конфликтовать с моделью fork. |
| **B. Сохранить current основной, переносить механизмы кластерами** | Current тоже достигает игры либо strict/gate проверки fork выявляют слишком много скрытой потери work/data | Сохраняются проверенные contracts, capture и regression corpus; каждый порт имеет понятную область проверки. | Runtime bindless затрагивает planner, emitter, bindings, residency, синхронизацию и lifetime. Два marker-commit без базового кластера не дадут готового решения. |

Предпочтение — A как следующий эксперимент, с решением о production базе после
локального сравнения. Sharp использовать для семантики и независимых reproductions;
переписывание C# runtime в C++ или merge426 commits не является коротким путём.

## Что переносить в первую очередь

Это кандидаты для воспроизведения, а не уже доказанные дефекты нашей ветки.

| Приоритет | Кандидат и источник | Минимальный критерий до переноса |
| --- | --- | --- |
| P0 | Весь связанный runtime image-table кластер: [основа5155217d](https://github.com/peepostarexx/KytyPS5/commit/5155217dcb821887d13ac9a108b2aa04e83cd530), pointer/layer07f2e62a+cf47f345 | Divergent record/row, mutation поколения heap, stale feedback, invalid selected/unselected descriptors, реальные device limits. Для production нужны точные image+sampler operands и проверка выбранной ошибки до признания submission успешным. Miss не должен необратимо потерять side effects. |
| P1 | [Один S# для обычного и compare sample: f0bed387](https://github.com/peepostarexx/KytyPS5/commit/f0bed3873af224b4324aad66f28958daf21644ad) | Численный color/depth oracle с одним исходным S#, отдельные host samplers по употреблению, validation и border/integer соседи. |
| P1 | [Min/max sampler reduction:101988ab](https://github.com/peepostarexx/KytyPS5/commit/101988abdf799a5db099cdc4084230ba5a32f8ad) | Независимые texel values, min/max/обычный режим, swizzle/format соседи; feature gate и явный unsupported при отсутствии поддержки. |
| P1 | [Storage-only draw из Sharp](https://github.com/foufouadi/sharpemu/blob/95648c2b047fce1686345d09dce1a546fe0d35ee/src/SharpEmu.Libs/Gpu/Rendering/RenderExecutor.Targets.cs#L42) | Настоящий DrawAuto без framebuffer, store→GPU readback; zero-work, отключённый stale PS и обычный color/depth draw. Использовать существующий HasShaderMemoryWrites. |
| P1 | [GPU device-address stores/atomics и written-page reporting](https://github.com/foufouadi/sharpemu/blob/95648c2b047fce1686345d09dce1a546fe0d35ee/src/SharpEmu.ShaderCompiler.Vulkan/Gen5SpirvTranslator.Resources.cs#L1023) | Store в destination без bound writable binding→CPU read/upload/eviction; atomic, discard, границы страниц и no-store. Наш missing-page fault механизм уже есть, но это не доказательство отслеживания всех writes. |
| P2 | [Смешанные CPU/GPU записи одной страницы](https://github.com/foufouadi/sharpemu/blob/95648c2b047fce1686345d09dce1a546fe0d35ee/src/SharpEmu.Libs/Gpu/Buffers/SharedPageShadows.cs#L8) и [raw-read image alias deb04b1e](https://github.com/peepostarexx/KytyPS5/commit/deb04b1e55f314e8127dc20e414d5d401ec8c2d5) | Разные bytes одной page, упорядоченные конфликты, pending upload, рост owner и invalidation. Политика CPU-wins или потеря первого GPU write не берётся как спецификация. |
| P2 | Dispatch/draw snapshots и pixel history из fork | Сохранить полные inputs/outputs и provenance; replay не должен обращаться к живой памяти. Использовать один конкретный failing draw/dispatch. |
| P3 | NGG, wave64/LDS, tessellation, AGC markers, DCC/HTile | Порт только после конкретного отличия в одинаковом captured workload. У нас многие такие подсистемы уже реализованы; одинаковое название фикса не означает отсутствующий механизм. |

## Последовательность исполнения

1. **Зафиксировать контроль.** Сохранить current code3391a9fd, все artifacts и
   private SaveData. Новый comparison checkout и build directory создать отдельно
   от основной ветки. Не переписывать PR497 и не делать force-push.
2. **Один baseline fork.** Native Windows build точного6d041035; своя executable
   SHA, оригинальная01.512/eboot5178, копия одного SaveData, известные switches.
   Начать с documented defaults и обязательного linear-image readback. Не
   включать весь список экспериментальных flags одновременно.
3. **Один current warm control.** Та же игра, save, разрешение и точка запуска;
   code3391a9fd с existing optimizer/driver cache. Последний прерванный run не
   заменяет этот контроль. Сборки и игры строго последовательны; исходники
   заморожены, процессы ограничены временем/памятью и завершены с drain/dispose.
4. **Раздельно проверить результат.** Первые pixels; визуальное menu; вход в сцену;
   персонаж и реакция на ввод; затем одинаковый60-секундный участок. Записать время
   загрузки, median/p95 frame time, RAM/VRAM, resource misses, skipped draw/dispatch,
   validation errors.~7FPS чужого ПК не переносить в наш benchmark.
5. **Проверить цену fallback.** Отдельный bounded strict run fork с
   `KYTY_STRICT_TEXTURES=1` и `KYTY_STRICT_INDIRECT_CANDIDATES=1`; дополнительно
   считать materialization skips и sampler substitutions, которые эти flags
   сами не устраняют. Этот запуск диагностический, не тот же baseline.
6. **Выбрать A или B по наблюдениям.** Если fork даёт entry и current отстаёт,
   начать интеграцию от fork. Если current сравним либо fork держится на потерях
   обязательной работы, сохранить current и взять минимальный подтверждённый
   кластер. Если оба не работают, сначала один exact divergent capture,
   затем reproduction; не запускать серию новых фиксов по предположению.
7. **Переносить по одному контракту.** Synthetic intended RED→та же GREEN,
   необходимые GPU/neighbor проверки→один original retry. Для крупного bindless
   кластера сначала определить sampler, typed-view и fault/residency contracts;
   не подставлять default sampler/null для выбранного unsupported случая.

После переноса каждого кластера повторить тот же короткий игровой участок;
после failed comparison сохранять конкретный blocker и snapshot вместо новой
полной загрузки. Menu и gameplay остаются разными milestones. Другие игры,
если недоступны, заменять независимыми synthetic cases с явным пределом coverage.

## Что сохранить из нашей работы

Exact compile/resource capture и snapshot replay; конечные U32 selectors с
wrap/OOB negatives; численные wave64/LDS/resource tests; alias/ownership tests;
optimizer cache с exact input/recipe identity и native build/run tools.
Порты наших4f6fffda/257189e5 нужны только если эти механизмы применимы к новой
resource модели. Cache3391a9fd переносить после byte-equivalence проверки
optimizer recipe новой базы. Существующая Native Windows работа имеет ценность
как проверяемый corpus; это не обязательство сохранить весь production diff.

Не переносить без отдельного контракта shader/title skips, successful error
stubs, guessed row bounds, default samplers, первые32-record guesses, отключённую
validation или title-ID scheduling ветвления в общий renderer.

## Артефакты исследования

### Native fork baseline, 2026-10-11

Точный fork6d041035 собран на этом ПК, отдельные source/dependency worktrees и
runtime; EXE SHA256 b3ed953555e91759b798aced8a04af324508ceaafd25a70a34d6d34c879c96ef.
Оригинальная01.512/eboot5178, девять файлов того же SaveData seed, Performance1280x720,
linear-image readback/raw-alias-sync/bindless. Run133628 завершён по1800s bound
graceful exit0 в14:06:33UTC; memory guards не сработали, minfree9.86GiB.

Визуально подтверждены интерактивное вступление с touchpad prompt и меню
SETTINGS/CONTROLS после настоящего Enter/Options. Снимки1404 иbefore-timeout
в `_Build/runs/game-20261011-133628-fork-6d041035-baseline-fb3974`.
На снимках title counter6.2/9.1FPS, это не60-секундный benchmark и не проверка
движения персонажа. Main-menu/title screen и gameplay остаются отдельными
непроверенными стадиями. Эти результаты относятся к fork, а не к PR497.

PS617c присутствует в journal; сохранён driver cache265360493bytes. Лог содержит
sampler substitutions, placeholders, включая реально запрошенные ресурсы, и
partly-unmapped zero-fill warning. Результат подтверждает пользу fork как
доступного native эталона для сравнения; корректность этих fallback не доказана.
Current paired retry с полным pre-generation capture ещё требуется до решения
о production базе. Полный отчёт и build/run provenance:
`_Build/analysis/yotei-fork-verification-20261011`.

Три независимых read-only сравнения и pinned API/source snapshots сохранены под
`_Build/analysis/yotei-strategy-reset-20261011/`: `peepo/branch-review.md`,
`sharp/fresh-comparison.md`, `current-branch-review.md`. Это исследование,
не native запуск этих upstream heads. Следующий полезный результат — локальный
paired baseline, после него решение о базе. Дополнительные мелкие исправления
текущего PS до такого сравнения сейчас не имеют приоритета.
