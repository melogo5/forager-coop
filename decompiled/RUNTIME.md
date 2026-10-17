# Рантайм GameMaker внутри Forager.exe

Для нативного патча (`native/`). Билд игры 2.2.2.302 (в меню — 4.1.9), **рантайм GameMaker 2.2.5.378** (до 2.3:
нет структур и методов). x86, PE timestamp `0x60148428`, ASLR включён: адрес в процессе = VA − `0x400000` + база модуля.

Все VA ниже — для базы `0x400000`. Проверено зондом 2026-10-16 (см. «Проверено в игре»).

## Соглашения о вызовах (cdecl)

| Что | Сигнатура |
|---|---|
| Скрипт `gml_Script_X` | `RValue* (CInstance* self, CInstance* other, RValue* result, int argc, RValue** args)` |
| Событие `gml_Object_X_Event` | `void (CInstance* self, CInstance* other)` |
| Встроенная функция (из таблицы) | `void (RValue* result, CInstance* self, CInstance* other, int argc, RValue* args)` |

## Структуры

**RValue** (16 байт): `+0` значение (double / int32 / int64 / указатель), `+8` flags, `+0xC` kind.
Kind (`& 0xFFFFFF`): 0 real, 1 string, 2 array, 3 ptr, 5 undefined, 6 object, 7 int32, 10 int64, 13 bool,
14 iterator. Память есть только у kind 1..3 — их освобождает `FREE_RValue`.

**CInstance**: `+0` vtable (`[1]` = `InternalGetYYVarRef(slot)`, thiscall), `+4` `RValue* yyvars` (переменные
по слоту, шаг 16 байт; может быть NULL — тогда через vtable), `+0x74` flags (биты 0/1 — уничтожен / деактивирован),
`+0x7C` object_index, `+0x130` следующий активный экземпляр.

**Объект** (ресурс): `+0x44` связный список экземпляров `{next, ?, CInstance*}`.

**YYVAR** `{const char* name; int id}` в `.data` — id переменной или функции заполняется рантаймом при старте.
Карта адресов — `code/yyvar_map.tsv`.

**RFunction** (таблица встроенных функций, 0x4C байта на запись): `char name[64]`, `+0x40` fn, `+0x44` argc.
В этом билде 2502 записи.

## Адреса

| VA | Имя | Примечание |
|---|---|---|
| `016AB490` | `YYGML_CallBuiltin(self, other, &ret, argc, fnIndex, RValue** args)` | копирует аргументы, вызывает `table[fnIndex].fn` |
| `016CAD50` | `Variable_GetValue(int instId, int slot, int arrayIndex, RValue* out)` | instId: ≥100000 — экземпляр, <100000 — объект, −3 — all |
| `016CAF50` | `Variable_SetValue(int instId, int slot, int arrayIndex, RValue* val)` | то же |
| `016CA140` | `Variable_GetBuiltin(CInstance* self, int varId, int arrayIndex, RValue* out)` | `x`, `y`, `id`…; arrayIndex `0x80000000` = без индекса |
| `004012B0` | `FREE_RValue(RValue*)` | |
| `00409120` | `RValue::operator=` (thiscall, this = dst) | копирование с учётом счётчика ссылок |
| `016A52A0` | поиск экземпляра по id | хеш `DAT_01AE8E5C` / маска `DAT_01AE8E60` |
| `016AADE0` | `YYError(fmt, …)` | фатальная ошибка GML |
| `016ABC10` | `YYSetString(RValue*, const char*)` | новая строка GML (kind 1, счётчик 1) |
| `016CACD0` | `Variable_SetBuiltin(CInstance*, varId, arrayIndex, RValue*)` | запись `x`, `sprite_index`… |
| `016F2D00` | `event_user(self, other, n)` | = `Perform_Event(self, other, 7, 10+n)` |
| `017DA800` | `Perform_Event(self, other, type, number)` | |
| `016EC110` | `YYGML_instance_destroy(self, other, argc, RValue** args)` | YYC вызывает напрямую, не через таблицу |
| `016E6BA0` | `instance_destroy` (ядро): `(self, other, id, executeEvent)` | id = −1 — сам объект; сюда сходятся все уничтожения |
| `01B8C014` | `RFunction*` таблица встроенных функций | |
| `01B7CA98` | `CInstance*` глобальный объект (`global`) | |
| `01B77490` | вершина стека трассировки YYC | каждая функция кладёт своё имя |
| `01B8E0D0` | текущая комната; `+0x80` — первый активный экземпляр | |

Адреса скриптов и событий игры — `code/index.tsv`; номера слотов переменных — `code/var_slots.tsv`.

## Проверено в игре (зонд `native/src/probe.cpp`)

- Прокси `version.dll` загружается, хуки MinHook на `gml_Script_PlayerFind` и `gml_Object_objPlayer_Step_0` работают.
- Работают чтение `x`/`y`/`id` игрока, глобальной переменной по слоту (`global.players`) и вызов встроенной
  функции по имени (`ds_list_size`).
- **`PlayerFind` всегда вызывается с n = 0** (≈1,4 млн вызовов за 4 минуты, других значений нет) — это подтверждает
  вывод из FINDINGS.md. Он вызывается ~350 раз за кадр, поэтому хук на нём должен быть очень дешёвым.
