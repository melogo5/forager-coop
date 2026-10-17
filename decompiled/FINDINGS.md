# Forager: как устроены игрок, состояние и мир (для coop)

Выводы по псевдокоду билда 2.2.2.302. Ссылки — на файлы в `code/`.

## 1. Игрок — элемент списка, но фактически один

- `objPlayer` (родитель `parAlly`) в `Create_0` делает `ds_list_add(global.players, id)`,
  в `CleanUp_0` удаляет себя из списка.
- `PlayerFind(n)` = `ds_list_find_value(global.players, n)` ([scripts/PlayerFind.c](code/scripts/PlayerFind.c)).
- **365 функций** получают игрока через `PlayerFind(<константа>)`. Список `global.players`
  больше нигде не используется — ни одного цикла по игрокам. При одном игроке любой индекс
  кроме 0 вернул бы `undefined`, значит везде фактически `PlayerFind(0)`.
- Это след отменённого официального мультиплеера: абстракция есть, логики для N игроков нет.

## 2. Где живёт состояние

**На экземпляре игрока** (читается через `PlayerFind(0)`):
`inventory`, `inventorySize`, `maxSlots`, `coins`, `hp`, `invulFrames`, `energy`, `level`,
`levelMax`, `xpToGain`, `gear`, `gearCurrent`, `tool`, `toolSelected`, `buff`, `cosmetic`, `bomb`,
`rodCharge`/`rodCooldown`, `toolbarGearWhitelist`.

**Глобально:** `skillUnlocked` (навыки), `featProgress`/`featUnlocked` (достижения), `droids`,
`assignedDroids`, `ownedLands` (купленные острова), `zoneCurrent`, `options`, `deathCount`,
`playerData`, `godMode`.

Ключевые функции: `GainItem` / `RemoveItem` → `inventory`, `coins`; `DmgPlayer` → `hp`,
`invulFrames` (24× `PlayerFind`); `GainXp` → `level`, `xpToGain`; `HasSkill` → `global.skillUnlocked`.

## 3. Ввод глобальный

`InputCheck`, `InputCheckDown`, `InputCheckPressed` читают `global.inputStatus`, параметра
«чей ввод» нет. `objPlayer/Step_0` (28k инструкций, только `.summary.txt`) читает ввод напрямую.

## 4. Почему второй `objPlayer` не подходит

Мод может вызвать `instance_create_depth(..., objPlayer)` — экземпляр встанет в `global.players[1]`. Но:
- он будет реагировать на тот же глобальный ввод, что и первый;
- его собственные `Step_0`, `Step_2`, `Draw_0` сами вызывают `PlayerFind(0)` — часть логики
  и отрисовки будет брать состояние первого игрока;
- весь остальной код (урон, предметы, XP, UI) видит только игрока 0.

**Вывод:** второй игрок — «марионетка» на `ModObjectSpawn` (спрайты `sprPlayer*` + косметика),
его действия исполняются модом через API (`GainItem`, урон по объектам и т.п.).

## 5. Камера и culling — одна область

`CullObject` деактивирует объекты вне `global.cameraLeft/Right/Top/Bottom`
(вызывается из `WorldControl/Step_1`). Пропускает объекты с `antiCull` и `tempActive`.
У хоста мир вокруг второго игрока будет деактивирован → нужно держать область активной
(`antiCull`/`tempActive` для объектов рядом с марионеткой) или симулировать его зону на клиенте.

## 6. Мир спавнится случайно и по таймеру

`ResourceSpawn` вызывают `WorldControl/Step_0`, `VoidControl/Step_1`, `objBigResSpawner/Step_0`.
Он выбирает случайные точки на `global.ownedLands`, учитывает биом и `zoneCurrent`,
создаёт объекты через `instance_create_depth`, моды — через `ModEnemySpawn`.
Дроп — `DropItem` из `Destroy_0` десятков объектов.

→ Мир должен быть **авторитетным на хосте**: клиент гасит собственный спавн
(`OnResourceSpawn`, `OnMobSpawn`) и создаёт объекты по сообщениям хоста с сетевыми ID.

## 7. Сохранения

`SaveGame`, `SerializeCharacterChunk` (27× `PlayerFind`), `DeserializeSaveMap` — сохраняют
одного игрока. Данные марионетки придётся хранить отдельно (`ModSaveDataSubmit`/`Fetch`,
`OnSave`/`OnLoad`).

## Итоговая модель coop

| Что | Где | Подход |
|---|---|---|
| Мир, ресурсы, мобы, постройки | хост | хост решает, клиент отображает |
| Игрок хоста | `objPlayer` хоста | как есть |
| Игрок клиента у себя | `objPlayer` клиента | как есть; действия → хосту |
| Чужой игрок | марионетка `ModObjectSpawn` | позиция/анимация по сети |
| Инвентарь, HP, XP | на `objPlayer` у каждого | раздельные — бесплатно |
| Навыки, достижения, острова | глобалы | общие — синхронизировать с хоста |

## Открытые вопросы

- Как именно `objPlayer/Step_0` наносит урон ресурсам (только `.summary.txt`/`.asm`) —
  нужен способ, которым клиент сообщает «ударил объект N», а хост применяет.
- Значения констант-аргументов `PlayerFind` задаются в рантайме; «всегда 0» — вывод
  из отсутствия циклов по `global.players`, а не прямое наблюдение.
- 46 функций без псевдо-C, в т.ч. `ModGMLInit` — точный список разрешённых модам функций
  оттуда не получен (есть косвенно: обёртки `__*` в `symbols/scripts.txt`).
