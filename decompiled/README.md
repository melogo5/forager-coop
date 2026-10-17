# Forager — выгрузка из билда игры

Материал для разработки coop-мода. Получен из локальной копии игры (Steam, app 751780).
`code/`, `data/` и `symbols/` — производные от проприетарного кода HopFrog — **не публикуются**:
в публичном репозитории их нет, их можно получить заново скриптами из `tools/` (раздел
«Воспроизведение»). Публикуются только наши заметки (`*.md`) и скрипты.

## Билд

| | |
|---|---|
| Версия (GEN8) | 2.2.2.302, конфиг `steam` |
| Дата сборки | 2021-01-29 (timestamp 1611956671) |
| Компилятор | **YYC**, x86 (PE32), `data.win` без чанков CODE/VARI/FUNC |

Из-за YYC исходного GML нет ни в `data.win`, ни в exe. Код существует только как нативные
функции в `Forager.exe`; ниже — их псевдо-C из Ghidra.

## Содержимое

### `symbols/` — имена из `Forager.exe`
- `objects_events.txt` — 327 объектов и их события (`objPlayer: Create_0, Step_0, …`)
- `scripts.txt` — 2237 скриптов, включая `__<builtin>` — обёртки GML-функций, доступных модам
- `mod_api_signatures.txt` — сигнатуры API модов (из встроенных подсказок)
- `mod_event_names.txt` — имена событий модов (`OnStep`, `OnNetworkAsync`, …; есть недокументированный `OnShutdown`)

### `data/` — метаданные `data.win` (UndertaleModTool CLI 0.9.2.0)
- `objects.tsv` — объект, родитель, спрайт, маска, флаги, события
- `rooms.tsv` — комнаты и размещённые в них объекты
- `extensions.txt` — расширения и DLL (`Forager_Scripting`, `extImguiGML`, `rousrDissonance`, …)
- `sprites.tsv`, `sounds.tsv`, `scripts.tsv`, `strings.txt`, `general.txt`

### `code/` — псевдо-C (Ghidra 12.1.4, headless)
- `objects/<объект>/<Событие>_<N>.c`, `scripts/<скрипт>.c`; collision-события — `Collision_<GUID>.c`
- `index.tsv` — имя → адрес функции в exe
- Из ~4080 имён привязаны к функциям 4015 (66 не найдены, напр. `objPlayer/KeyPress_17`);
  **3969 декомпилированы**, 46 — нет (таймаут 300 с или >14 ГБ RAM у декомпилятора).
- Для этих 46 рядом лежат `<имя>.asm` (полный листинг) и `<имя>.summary.txt`
  (вызываемые функции и строки). Файл `.c` у них содержит только `// decompile failed`.
  Важные среди них: `objPlayer/Step_0`, `Step_2`, `Draw_0`; `WorldControl/Step_2`, `Draw_64`;
  `ModGMLInit`, `ModConstantInit`, `GameInit`, `SaveGame`, `DeserializeSaveMap`;
  остальные — интерпретатор модов (`gml_*`, `vm_gml_thread_exec_*`) и UI-меню.
- В сводках `FUN_xxxxxxxx` — безымянные функции рантайма GameMaker, `gml_Script_*` — скрипты игры.

**Аннотации** (`tools/annotate_yyvars.py`, применены ко всем `.c`):
- `ds_list_add.id`, `x.id` — встроенные функции/переменные GML (было `DAT_xxxxxxxx`), таблица — `yyvar_map.tsv`
- `GETVAR(self, inventory)` / `VARSLOT(self, inventory)` — доступ к переменной экземпляра по слоту;
  `Variable_GetValue(inst,inventory,…)` / `Variable_SetValue(…)` — к переменной другого экземпляра.
  Слоты → имена: `var_slots.tsv` (2349 переменных)
- `global` — глобальный объект; `YYGML_CallBuiltin`, `FREE_RValue` — функции рантайма

Выводы для coop — в [FINDINGS.md](FINDINGS.md).

Имена функций восстановлены так: каждая YYC-функция записывает указатель на строку своего
имени (`gml_Object_objPlayer_Step_0`) в стек-трейс, поэтому функция, содержащая ссылку на эту
строку, и есть искомая. Вызовы других GML-скриптов внутри псевдокода тоже переименованы.

Как читать YYC-псевдокод: переменные инстанса и глобалы идут через `YYGML_*`/`Variable_*`
с числовыми ID, строки — через указатели в `.rdata`. Логика читается, но требует терпения.

## Что важно для coop (сводка)

- Сеть модам доступна: `network_create_server/socket/connect/send_packet/send_broadcast/destroy`,
  `buffer_create/write/read/tell/delete`, `ds_map_*`, `http_get/post_string`, событие `OnNetworkAsync`.
- Нет: `buffer_seek`, `network_send_udp`, `network_send_raw`, `json_encode` → протокол на TCP,
  новый буфер на пакет, сериализация вручную.
- Интерпретатор модов: скрипты `Mod*`, reflection-слой `ref_*_init_*`, `sr_*`.

## Воспроизведение

Скрипты сборки — в `tools/`:
`dump_symbols.py`, `build_name_map.py`, `ghidra_scripts/ExportYycFunctions.java`
(аргументы: карта имён, выход, [список имён], [таймаут]), `ghidra_scripts/ExportFunctionSummary.java`,
`DumpMetadata.csx`. Проанализированный проект Ghidra (~440 МБ) — `../ghidra/Forager.gpr`
(в `.gitignore`; открывается в Ghidra GUI через File → Open Project). Повторные прогоны:
`analyzeHeadless <proj> Forager -process Forager.exe -noanalysis -readOnly -postScript …`.
