# Ghost of Yōtei: исследование подхода к отладке и план разработки

Проверено 10 октября 2026 года, 13:32 Europe/Minsk / 10:32 UTC. Основные внешние источники: **boykopovar/AnyPS5** и **SharpEmu PR #926**, как уточнил пользователь. Время в локальных логах ниже — UTC.

**Рекомендуемый путь: сначала сохранить полный контекст отказа и сделать автономный replay, затем доказать реально достижимые индексы ресурсов.** Если CPU-доказательство множества индексов для конкретной формы невозможно, отдельным этапом внедрять модель runtime-таблиц с обязательной ошибкой при использовании ошибочной записи, по образцу AnyPS5 #757. Кэширование и оптимизацию драйверной компиляции рассматривать после воспроизводимого корректного результата.

Этот документ — исследование и предложение. Новые механизмы захвата, replay, runtime-таблиц и перечисленные переносы в этой работе не реализованы. Исходники эмулятора не изменены. Проверены текущие код, история, артефакты и первичные GitHub-источники; оба внешних эмулятора здесь не запускались.

## 1. Что установлено о меню и предполагаемом регрессе

| Эксперимент | Проверенный результат | Практический вывод |
| --- | --- | --- |
| Историческая b4eae640, EXE 87f24, оригинальная игра | Снимок window-medium-ready.png действительно показывает CHANGE DIFFICULTY / MEDIUM на фоне сцены. В следующем запуске той же сборки подтверждён Quality. | Первоначальный экран настройки раньше работал. Нельзя писать, что Kyty никогда не показывал меню. Это не доказательство входа в игру. |
| Та же историческая сборка после сохранения Quality | Запуск 20261006-011540 остановился на CS34be: 33 сэмплера превышают старый лимит 32. | Уже старая сборка не доказывала меню при следующей загрузке сохранённого состояния. |
| Пересобранная b4eae640, отдельный runtime, копия нынешних SaveData и кэша, None, 2560×1440, redzone | Запуск 20261010-094629 завершился естественно с 321 через 14:10; тот же CS34be / pc05e8 / sampler33. Меню не показано. | Совпадающий эксперимент повторил исторический отказ старого кода после настройки. Пересобранный EXE 0c60 отличается от исторического 87f24; это явно учтено. |
| Текущий EXE 454656 / source a593e9b0 при той же исходной копии состояния и параметрах | CS34be пройден, pipeline29 создан за 249 мс. Запуск 20261010-100152 завершился с exit1 через 17:09; последний этап — начало CS54904 pipeline242. Причина завершения не установлена; timeout и memory guards не сработали. | Исправление сэмплеров продвинуло этот сценарий. Этот запуск нельзя объявлять ни успешным меню, ни воспроизведением PS617c, ни доказательством отсутствия всех регрессий. |
| Текущий source a593e9b0, Performance, 1280×720 | Завершённый diagnostic run 20261010-092136 проходит фактический PS617c tracking, затем отвергает запись таблицы record0 / row64. | Это последний точно диагностированный ресурсный блокер в указанной конфигурации. |

В последнем отказе источник содержит два record со stride136; row_stride368; numeric guard допускает индексы меньше 255. Материализатор перебирает весь диапазон 0…254. Строка 64 имеет зарезервированный тип изображения 4 и данные, похожие на обычные значения, а не T#. Предыдущие строки проверку прошли, включая возможные настоящие null-descriptors. **Из этого нельзя вывести допустимый предел 63 или доказать недостижимость row64.** Нужно восстановить происхождение индексов и принадлежность реально читаемой памяти.

Разбор сохранённого IR показывает byte extraction, сдвиги на 8, Phi, UMin и wave reduction. Возможный sentinel255 отличается от длины таблицы. Гипотеза: числовой guard ошибочно принят за полное множество ресурсов. Альтернативы — неверная provenance указателя, повреждение или несвоевременное чтение данных. Они пока не исключены.

Первоначальный setup, загрузка после Quality, SPIR-V optimization и сохранения менялись между экспериментами. **Конкретная кодовая регрессия потери первоначального setup не локализована.** Следующая проверка — отдельная baseline/head пара на чистом first-run state, затем одинаковые настройки. При подтверждённой потере одного и того же экрана — бисекция семантических изменений между последней работающей и первой неработающей ревизиями.

Артефакты: [контролируемое сравнение](../_Build/analysis/menu-regression-investigation-20261010/investigation.json), [результат baseline](../_Build/analysis/menu-regression-investigation-20261010/baseline-saved-none-result.json), [head run](../_Build/runs/game-20261010-100152-menu-head-saved-none/run.json), [точный ресурсный отказ](../_Build/runs/game-20261010-092136-wave-image-descriptor-diagnostic/stdout.txt). Оригинальные SaveData не сбрасывались; эксперименты используют отдельные копии. Fresh-runtime каталоги подготовлены, но соответствующая пара ещё не запускалась.

## 2. Что в текущем процессе следует изменить

### Сохранять весь отказ, а не только код шейдера

Действующий capture сохраняет code и plain metadata. Полные runtime contents не сохраняются. В обычном ShaderBatchAudit глубокий путь реализован для compute; noncompute остаётся CFG-only. PS617c был разобран временным диагностическим режимом с default pixel inputs. Его успех не закрывает materialization, реальный pipeline layout или исполнение.

После изменения синтетического механизма игра обнаруживает следующий недостающий контекст, и требуется ещё одна длительная загрузка. Нужно завершить связку **реальный failure bundle → автономное воспроизведение → синтетический эквивалент → общий фикс → тот же replay → GPU → игра**.

### Разделять числовой диапазон и множество возможных значений

Guard, sentinel, NumRecords одного источника и размер другой таблицы — разные сведения. Для wave-selected адресов необходимо сохранить связь **record, row, маска EXEC, происхождение pointer и lifetime**. Перебор прямого произведения диапазонов без доказательства их связи способен читать ресурсы, которые guest никогда не выбирает.

Синтетические RED/GREEN остаются обязательными. Их нужно расширять границами, отсутствовавшими в первой модели, а не считать проход первой удобной формы полной поддержкой игры.

### Сделать конечный результат эксперимента обязательным

Каждый результат должен содержать revision, dirty fingerprint, точный EXE, зависимости, seed hashes сохранений и кэша, параметры, фазу, причину выхода и cleanup. Сейчас helper пишет общий result.json при PASS; failure/input_error/timeout должны оставлять такой же итоговый документ. Списки процессов и общий execution-slot lock у runner, audit и batch нужно согласовать.

Последний head/None завершился с exit1 без установленного объяснения. В ограниченном окне событий Application1000/1001/1002 записи по kyty_emulator не найдены. Это не доказывает ни драйверный crash, ни остановку пользователем. Подобный результат обязан оставаться «причина неизвестна», пока не получено событие или точный termination trace.

## 3. Где сейчас тратится время

| Измерение по сохранённым артефактам | Стоимость | Область доказательства |
| --- | --- | --- |
| 14 завершённых игровых запусков 10 октября | 301,96 минуты суммарно; 14:10…40:33 на запуск | Включает timeout и memory guard; это стоимость экспериментов, а не 14 корректных воспроизведений ошибки. |
| Восемь последовательных frontier retries 033329…092136 | 157,29 минуты; все закончились321 | Повтор загрузки до очередного отказа. |
| PS617c diagnostic CPU audit085935 | 0,296 с; сборка16,201 с | Tracking с неполными default pixel inputs; не materialization/GPU. |
| Mask-cycle RED/GREEN | 0,113 / 0,059 с | Узкая синтетическая проверка. |
| Resource suite090014 / GPUAV090033 | 4,120 / 0,794 с | Указанные CPU-контракты и три соседних GPU-кейса. |
| Нативная baseline сборка765шагов | 106,283 с | Сборка старой ревизии с отдельными зависимостями. |
| Три CS54904/None permutation в head replay | 135,303 / 142,615 / 145,003 с на driver creation | Различные варианты; нельзя считать их дубликатами только по guest hash. |

Времена фаз могут перекрываться; их нельзя складывать и выдавать за длительность запуска. Практический приоритет — убрать повторные boots ради уже сохраняемого отказа. Ожидаемый выигрыш измерить числом исключённых загрузок и временем до подтверждённого фикса; заранее обещать ускорение в N раз нельзя.

## 4. Что действительно известно о внешних источниках

### AnyPS5

Проверенный main: **a29ef592190d0450eacbde15ec8e228e379ed753**. Старый локальный обзор d70b8998 не описывает все новые изменения.

[PR2349](https://github.com/boykopovar/AnyPS5/pull/2349), head **8dbea2c6548521147027cbe9b7759643a0239fb2**, merged по live API 10 октября09:00UTC. Автор сообщает первые представленные кадры SIE intro Yōtei на Windows/RTX4090 после выделения shared DontInline helper для GPU-selected formatted accesses. Указано уменьшение одного модуля с37,1M до1,45M SPIR-V words. Это авторская проверка intro/pixels, не подтверждение меню или gameplay в исследованных материалах.

[Таблица совместимости](https://github.com/boykopovar/AnyPS5/blob/a29ef592190d0450eacbde15ec8e228e379ed753/docs/user/COMPATIBILITY.md) перечисляет Dreaming Sarah. Значок1166/1166 относится к инвентарю enum/alias, а не к численным тестам всей ISA: [progress.py](https://github.com/boykopovar/AnyPS5/blob/a29ef592190d0450eacbde15ec8e228e379ed753/tools/progress.py).

### SharpEmu PR926

[PR926](https://github.com/sharpemu/sharpemu/pull/926) обновился во время исследования. Проверенный live head: **d6a883196f0c8616299108c6841fcb3bc36b5154**, open, draft=false,420commits/379files. Локальный reference архив остаётся на848fceef. Дополнительно проверен [delta16commits/35files](https://github.com/sharpemu/sharpemu/compare/848fceefbd409444e2236d7438571641d603dafb...d6a883196f0c8616299108c6841fcb3bc36b5154).

Автор сообщает меню, gameplay и отображение персонажа на Windows/RTX5070; [runtime report/screenshots](https://github.com/sharpemu/sharpemu/pull/926#issuecomment-6045235311). Изображения этого сообщения в текущем инструменте не удалось визуально проверить. Точная ревизия авторского запуска не установлена. Новейшие изменения не являются нашим independently-tested working build; PR также описывает проблемы потери данных/затемнения.

Следовательно, SharpEmu полезен как сравнение реализации и поведения. Запуск этой ветки у нас для проверки одинакового сценария остаётся отдельной задачей, если её сборка и данные доступны.

## 5. Что брать и что уже взято

| Источник и механизм | Польза для Kyty | Предлагаемое действие |
| --- | --- | --- |
| AnyPS5: семь SOP1, integer64 predicates, runtime raw DWORD stride, formattedX и wide formatted transfers | Адаптации уже находятся в истории текущей ветки: f20f2bd8,6e401f7c,8c5beaa1,4d5a2376,7afac3cd. | Сохранять численные регрессии. Новая задача — только отсутствующий семантический случай, подтверждённый отказом. Повторный полный перенос этих частей не нужен. |
| Sharp926: конечное множество selector offsets | Ближе к текущему row64, чем полный числовой диапазон. Recognizer926 поддерживает ограниченные FirstLane/bitscan/moves/add/shift-left/clean-SMEM формы. | Использовать контракт и архитектурную идею. Текущего packed-byte/UMin/shift-right8 доказательства там нет; delta d6a его не добавил. [Pinned selector code](https://github.com/foufouadi/sharpemu/blob/d6a883196f0c8616299108c6841fcb3bc36b5154/src/SharpEmu.ShaderCompiler/Resources/IndirectSelectorValues.cs). |
| Sharp926: ограниченная диагностика через уже выполненные reads | Фиксирует selection mode, failure, selectors/offsets, первый failed address, до256read records и omitted count. | Перенести способ сбора в наш failure bundle. Тест должен проверять те же calls/results без дополнительных синхронизаций. [IndirectImageFailure](https://github.com/foufouadi/sharpemu/blob/d6a883196f0c8616299108c6841fcb3bc36b5154/src/SharpEmu.ShaderCompiler/Resources/IndirectImageFailure.cs). |
| AnyPS5 #757: runtime image/sampler tables с per-entry fault | Позволяет отличить ошибочную неиспользуемую запись от фактически выполненного неправильного доступа. | Основа варианта B ниже. [Pinned emitter](https://github.com/boykopovar/AnyPS5/blob/4854d5888ef1eb9cceb4f8a6c6fbddc0cc6512c2/core/shader/recompiler/SpirvBackend/src/SpirvImageEmitter.cpp), [execution tests](https://github.com/boykopovar/AnyPS5/blob/4854d5888ef1eb9cceb4f8a6c6fbddc0cc6512c2/core/libs/prx/libSceAgcDriver/tests/execution/ImageTable.cpp). |
| Sharp926: storage-producing draw без framebuffer exports | В renderDraw.cpp ещё есть ранний skip no-color/no-depth. При этом HasShaderMemoryWrites и barriers уже существуют. Связь с нынешним отказом не доказана. | P1: реальный DrawAuto RED с VS store; отдельно PS-only store, zero-work, disabled stalePS, depth/color соседями и sentinels. [RenderExecutor.Targets](https://github.com/foufouadi/sharpemu/blob/d6a883196f0c8616299108c6841fcb3bc36b5154/src/SharpEmu.Libs/Gpu/Rendering/RenderExecutor.Targets.cs). |
| Новые926: BDA-written-page bitmap, buffer-growth ownership, mixed CPU/GPU pages | Полезные наблюдения для потерь данных; корректное смешивание зависит от порядка writes/barriers. | P1/P2 после конкретного ownership RED. [BDA tracking](https://github.com/sharpemu/sharpemu/commit/d1aa198816317f2f0fa724e11fac37040efd2e39), [page merge](https://github.com/sharpemu/sharpemu/commit/837900d29e8a695e776df3b5d15964cbec45fc1d). Правило upstream CPU-wins нельзя принимать без контракта нашей синхронизации. |
| Новые926: padding последнего DWORD с точным guest byte bound | Возможные ошибки на коротких/невыравненных buffers. Автор связывает один случай с95-byte instance-state. | Проверять lengths mod4, backing, widths и OOB modes по ISA, затем совпадающий runtime. [Tail change](https://github.com/sharpemu/sharpemu/commit/8ab9894e9b7e4b28b3b2aa41107c86f7f1d38926). Связь с нашей игрой пока не установлена. |
| AnyPS5: ShaderDiskCache и полная сериализация artifact/metadata/bindings | Повторное CPU-планирование может быть исключено после стабилизации resource interface. | P2: corruption/truncation/key-sensitivity/invalidation/lifecycle tests. [Cache source](https://github.com/boykopovar/AnyPS5/blob/a29ef592190d0450eacbde15ec8e228e379ed753/core/shader/recompiler/ShaderDiskCache.cpp). Это не исправляет неправильный descriptor. |

Копирование whole backend не соединяет IR, ABI, scheduler, Vulkan layout и ownership двух проектов. Сохранять авторство и лицензионные заголовки при адаптации. Не переносить из Sharp926 null substitution stale/invalid entries и record_aligned_fallback в качестве обхода нашей ошибки. AudioOut2 offsets, ATRAC licensing и неизвестные register allowances требуют отдельных контрактов; они не приоритет этого отказа.

Уже имеющиеся Kyty DCC/CMASK/pitch, tessellation, независимые image/sampler bindings и Vulkan cache не следует описывать как полностью отсутствующие. Проверять только конкретные непокрытые края.

## 6. Два основных решения и дополнительный вариант

| Вариант | Суть | Когда выбирать | Стоимость и риск |
| --- | --- | --- | --- |
| **A — полный replay и доказанный live-selector domain. Рекомендуется первым.** | Сохранить реальные inputs/reads; восстановить packed-key provenance и record/row/EXEC связь; материализовать доказанное множество. | Когда selectors происходят из доступных стабильных данных и поддерживаемых операций. | Средняя/высокая сложность, локальнее замены resource interface. Риск — недостаточное доказательство текстурных/GPU-generated inputs. |
| **B — runtime-таблицы со строгим selected-entry fault.** | Разделить valid/null/poison entries; проверять выбранную запись на GPU; host обязательно принимает completion fault и отвергает draw/dispatch. Затем стабилизировать layout/variant keys. | Если A не может точно и безопасно представить динамический домен либо большие таблицы требуют другой модели. | Высокая сложность: emitter, descriptor negotiation, mappings, faults, lifetimes, barriers, cache identity. Делать в отдельной ветке по этапам. |
| **C — Sharp926 как поведенческий ориентир. Дополняет A/B.** | На отдельной сборке сравнить ту же игру и настройки, если возможно одинаковые device profile/state; получить минимальные traces offsets/writes/barriers. | Для различения ABI, resource-layout и scheduling гипотез. | Дополнительная подготовка сборки; C# и другой Vulkan слой не позволяют простой подмены частей Kyty. Одинаковое GPU/данные предпочтительны; чужой screenshot не заменяет локальный replay. |

### A: конкретная проверка текущего блокера

1. Сохранить фактические PS/VS inputs, resource roots, reads и writer/ownership контекст для PS617c.
2. Восстановить источник byte keys и условия их обновления: mask, Phi, shifts, minima, sentinel. Проверить принадлежность pointer/table memory и возможность CPU/GPU изменения данных. Не выводить размер таблицы из guard или первых валидных rows.
3. Создать минимальный CPU RED: sparse packed selectors, sentinel, некорректные/недоступные rows вне достижимого множества и корректные используемые rows. Отдельный negative — тот же invalid row реально достижим, materialization обязан отказать transactionally.
4. Сохранить record→row correlation, canonical address width, guest U32 wrapping, clean-reader/stage-write ограничения и quotas. Если чтение источника домена недоказано — явный отказ, а не guessed range.
5. Тот же GREEN, реальный captured materialization replay, SPIR-V validation, numerical wave32/64 GPUAV и affected corpus. Затем normal async original game.

### B: условия корректного переноса идеи AnyPS5 #757

В [PR757](https://github.com/boykopovar/AnyPS5/pull/757), head4854d588, ошибка связывается с активной EXEC и выбранными image/sampler entries. Автор отдельно не проверял PPSA26344 на этом PR. Это образец механизма и тестов, не готовый Yōtei fix.

Минимальный набор контрактов: invalid entry не выбран — правильные texels; invalid выбран при EXEC clear — отсутствие fault; при EXEC set — обязательный fault с PC/key/адресом/причиной; изменение table contents не оставляет stale mapping; self-write и poisoned nested SRT не теряются; budgets и host feature negotiation сохраняются.

Проверка должна происходить до обращения к poisoned descriptor или недопустимому host/BDA адресу. Fault привязывается к конкретному submission: нужны проверенные clear, lifetime, readback и обработка completion. Host обязан принять ошибку до признания результата успешным и не допустить принятого кадра или зависимого успешного результата из faulting work. Временный нулевой результат invocation допустим только внутри этого обязательного протокола ошибки. Один skip, null substitution или нечитанный fault нарушает контракт. Уже имеющийся buffer fault path — ориентир, но не доказательство image/sampler протокола. Stable bindless SPIR-V сам по себе не доказывает правильный набор ключей.

## 7. Как ускорить отладку и разработку

### Минимальный failure bundle

Сохранять bounded bundle за один осмысленный запуск: shader code; реальные CS/VS/PS stage inputs и связанное producer code; wave/ISA/host capabilities; userdata; static state/permutation/layout; source graph; canonical guest ranges и bytes уже необходимых reads; ownership/writers/barrier порядок; selector proof/rejection; revision/dirty fingerprint/EXE и seeds.

CPU replay читает remapped snapshots через bounded callbacks, без переноса live host pointers. Missing bytes/metadata дают input_error. Нельзя для завершения replay подставить нулевой ресурс. GPU replay — отдельный следующий шаг; CPU capture не восстанавливает всю игру или её scheduling автоматически. Proprietary captures остаются в ignored analysis; в tests идут синтетические аналоги.

P0-инструменты: единая команда CPU replay с фазами Track→Materialize→Specialize→Emit/Validate; отдельный bounded GPU режим; машиночитаемый итог на любом исходе; уменьшенный IR/source slice при rejection. Должны быть comparison tests, подтверждающие, что диагностический режим не меняет reads/results.

### Разделить цикл проверки

| Уровень | Когда выполнять | Выходной критерий |
| --- | --- | --- |
| Точный CPU failure replay | На каждой связанной итерации | Та же ошибка до фикса и тот же контекст после фикса; inputs полные. |
| Synthetic RED/GREEN | До/после production semantics | Независимый контракт, unchanged oracle и negative boundaries. |
| Bounded numerical GPUAV | После доказанного CPU изменения, затрагивающего execution | Значения, backing/sentinels, обе wave widths и relevant masks. |
| Affected corpus | На завершённом механизме | Группировка ошибок по общей причине; baseline failures отдельно. |
| Полный original boot | После закрытия предыдущих gates или ради недостающего диагностического контекста | Видимые pixels/setup/menu/game entry отдельно; точный blocker и cleanup. |

Не ждать полного boot для проверки decoder/IR/materialization, уже воспроизводимых автономно. Не повторять broad suite после unchanged PASS без новой причины. Native builds и GPU/game остаются последовательными; независимый analysis, source comparison и CPU batch после освобождения execution slot можно распределять между workers с явными владельцами файлов и лимитами.

### Оптимизировать измеренный дорогой путь

Kyty уже имеет совместимый Vulkan driver cache, in-memory module/permutation reuse, async compilation и measured-cost checkpoints. Повторное предложение «добавить кэш/async» не решает обнаруженный пробел.

Сначала сравнить exact SPIR-V + layout + specialization + fixed state у дорогих CS54904 variants. Guest hash один, варианты разные. Для одинакового варианта измерить cold/warm повтор. У persistent compiled-artifact cache ключ должен учитывать compiler/resource schema, code, все semantic inputs, host capabilities и validation/optimization mode; runtime snapshot остаётся отдельно с проверенным lifetime.

AnyPS5 #2349 полезен как проверка роста generated code: при добавлении accesses должны расти calls, а не повторяться большие switches/helpers. Подобные DontInline helpers у Kyty уже есть. Требуются собственные code-size regressions и driver timings; чужое уменьшение37M→1,45M нельзя выдавать за ожидаемый выигрыш наших688K модулей.

WATCH_BLOCKS/coherence trace из [нового926 diagnostic commit](https://github.com/sharpemu/sharpemu/commit/a09e559997bbdbbb42d24eb3ab0d1534fc2dc5e8) полезны как ограниченные observed ranges/ticks. Диагностические forced barriers меняют timing и подходят только для A/B локализации; они не становятся постоянным исправлением. Большой live debugger с неподтверждёнными data watchpoints сейчас менее приоритетен, чем полный resource replay.

## 8. План этапов и критерии решения

| Этап | Работа | Критерий перехода |
| --- | --- | --- |
| **P0.1 — завершить проверку регресса** | Fresh baseline/head с одинаковыми empty SaveData, DLLs, seed cache, redzone, None,2560×1440. Затем saved/Quality сценарий. Причину head exit1 уточнять только при наличии событий/termination evidence. | Один и тот же визуальный этап воспроизведён либо определена первая расходящаяся ревизия/фаза. SaveData пользователя сохранены. |
| **P0.2 — capture/replay** | Реальный PS617c bundle, полные stage metadata и bounded reads; общие run manifests и failure result. | Ошибка materialization воспроизводится без загрузки игры; replay не зависит от старого процесса/адресов. |
| **P0.3 — решение A** | Sparse packed domain RED/GREEN, source bounds/provenance, valid/invalid reachable distinction. | Точный capture и numeric GPU проходят; unsupported случаи всё ещё отказывают. |
| **Decision gate A/B** | Если домен нельзя безопасно представить из доступного контекста — зафиксировать недостающую capability. | Обоснован отдельный B prototype; дальнейших guessing/guard-only расширений нет. |
| **Интеграция сразу после каждого законченного A/B механизма** | Normal async original game; visible setup/menu, затем game entry. Не откладывать retry до исправления других кандидатов или кэширования. | Снимки и фактический ввод/entry; pipeline counters не подменяют эти milestones. Новый blocker записывается отдельно. |
| **P1 — renderer/data producers, если связан с новым отказом** | No-framebuffer storage draw regression; по подтверждённым captures — tail bounds и BDA ownership. | Observable writes и synchronization проверены; изменение привязано к реальному producer, не только аналогии с другим репо. |
| **P2 — последующая оптимизация повторной работы** | После correctness gate: exact-key variant reuse, compiled artifacts cache, code growth и cold/warm metrics. | Снижение собственных затрат без изменения numerical results, errors, command ordering и lifetime. |

Для планирования A — несколько отдельных циклов инструментария/контракта, B — более крупная серия renderer/backend изменений. Точный срок назначать после первого полного replay: пока источник всех packed keys не доказан, оценка в днях была бы ненадёжной.

Поддерживать короткий текущий статус и один actionable next blocker. Исторические checkpoint сохраняются как приложение/история; они не должны скрывать текущее состояние процесса. Каждая законченная семантическая правка — отдельный commit с RED/GREEN и integration record. Push и публикация issue108 ограничены текущей авторизацией; восстановление старого setup не повод дублировать прежний milestone. Другой игровой runtime/corpus здесь отсутствует, поэтому synthetic coverage не называется cross-game compatibility.

## 9. Проверяемые локальные записи

- [Текущий launch plan](ghost-of-yotei-windows-plan.md) и [незакрытые контракты](emulator-test-debt.md).
- [Предыдущий AnyPS5 audit](anyps5-usefulness-review.md) и [предыдущий Sharp926 audit](sharpemu-pr926-usefulness-review.md): исторические pins; свежие отличия перечислены выше.
- [Нативные checks](../_Build/checks/), exact run.json/stdout/stderr в упомянутых runs.
- [Research manifest](../_Build/analysis/yotei-research-plan-20261010/research-manifest.json): live API pins, источник и состояние процессов на момент проверки.

Рекомендуется начать с P0.1/P0.2 и варианта A. Вариант B подготовить как самостоятельный технический путь при установленном ограничении статического домена; C использовать для точного сравнения спорной семантики. Следующим критерием успеха становится воспроизводимый captured-resource результат и восстановленный видимый этап при одинаковых inputs, а не очередной успешно разобранный shader.

## Выполнение плана: 2026-10-10 12:53 UTC

P0.1: чистый старый runtime дошёл до показанного пользователем Digital Deluxe Bonus, затем отказал на старом SDL_OpenGamepad assertion. Чистый текущий None runtime завершён по30-минутному лимиту без подтверждённого нового меню. Состояние сохранений действительно меняет путь загрузки; исходный source regression ещё не изолирован.

P0.2 для нынешнего PS617c завершён: actual run121651 дал18176-byte код, полные PS inputs/userdata и740 ordered callbacks. CPU RED123614/124343 воспроизводит ровно тот же record0row64 failure за~0,182с, без live/GPU reads, вместо~19 минут загрузки игры. Recorder ограничен8MiB/131072events, сохраняет исходные результаты и не добавляет чтений. Этот transcript покрывает прежний алгоритм; дополнительное чтение текстуры после будущего фикса будет отсутствующим input. VS/compute runtime replay и back-code пока не реализованы. Новая запись включает device limits для всех стадий; ранний actual PS bundle их не имел, что audit явно отмечает; они не потребляются нынешним PS frontend и GPU replay не заявлен.

IR показывает происхождение row из ImageRead RGB byte IDs через Phi/Select, shift8, UMin и DPP/ReadLane31,63. Source4/logical1: descriptor offset16 в record stride136; фактические входные изображения RGBA8UInt513x513,Standard4KB,mip0,identity swizzle. Поля T# записаны, содержимое пикселей — ещё нет. План адресной таблицы показывает pointer_offset0, row_stride368, guard255. Число63 в соседних данных не является контрактом для жёсткого ограничения. Следующий шаг A требует независимых REDs для provenance/byte-set/tiling/coherence/writer overlap и корреляции record-row; только после этого — дополнительный полный input capture и semantic fix. B остаётся самостоятельной альтернативой с обязательным host-consumed selected-entry fault.

Управление при английской раскладке и фокусе окна: J=Cross/подтвердить, L=Circle/назад, стрелки=D-pad, Enter=Options, I=Triangle, K=Square. Текущая игра ещё не подтверждена как работающая; новые menu/game-entry milestones не публиковались.
