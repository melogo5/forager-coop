# forager-coop

Кооператив на двоих для [Forager](https://store.steampowered.com/app/751780/Forager/) как нативный
патч: одна `version.dll` в папке игры. Игра по LAN, Hamachi или Radmin, подключение через новый пункт
главного меню **COOP**.

> **Статус: прототип, разработка продолжается.** Основная игра вдвоём работает
> (мир, добыча, мобы, постройки, сундуки, покупка островов), но есть известные рассинхроны —
> см. [docs/coop/STATUS.md](docs/coop/STATUS.md).

## Как это устроено

Forager собран YYC (GML скомпилирован в машинный код), поэтому кооп встроен через перехват функций
игры: `version.dll` проксирует системную библиотеку и ставит хуки
([MinHook](https://github.com/TsudaKageyu/minhook)) на функции `Forager.exe`.

- **Хост авторитетен:** мир живёт и сохраняется только у хоста; гость получает снимок сейва при
  подключении и дальше — изменения (появление/уничтожение объектов, удары, мобы, острова).
- **Напарник** — второй `objPlayer` с отключённой логикой («марионетка»); действия гостя хост
  исполняет от его имени.
- **Персонаж гостя** (инвентарь, уровень, навыки) хранится у хоста под именем гостя.
- Смерть — возрождение на стартовом острове, без game over.

Подробно: [docs/coop/SPEC.md](docs/coop/SPEC.md) (что строим),
[docs/coop/ARCHITECTURE.md](docs/coop/ARCHITECTURE.md) (как),
[decompiled/RUNTIME.md](decompiled/RUNTIME.md) (рантайм GameMaker изнутри).

## Требования

- У каждого игрока своя копия Forager из Steam. Поддерживается только билд с PE timestamp
  `0x60148428` (Steam-версия 2021-01-29, в меню — 4.1.9); на другом билде DLL ничего не перехватывает.
- Windows, оба игрока в одной сети (LAN или VPN вроде Hamachi/Radmin). Порты: TCP 7790, UDP 7791.
- Моды игры (Mod API) не поддерживаются.

## Сборка и установка

Нужны Visual Studio Build Tools с C++ (x86). MinHook — подмодуль:

```powershell
git clone --recursive https://github.com/melogo5/forager-coop.git
cd forager-coop
powershell -File native\build.ps1 -Install -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Forager"
```

`-Install` копирует собранную `native\build\version.dll` в папку игры (`-GameDir` — ваша папка Forager).
Тестовые скрипты в `tools/gametest/` рассчитаны на машину разработчика (пути внутри).

Удаление — стереть `version.dll` из папки игры. Как играть — [native/README.md](native/README.md#как-играть).

## Структура

| Папка | Что внутри |
|---|---|
| `native/` | исходники `version.dll` |
| `docs/coop/` | спецификация, архитектура, статус, решения и известные проблемы |
| `decompiled/` | заметки о внутреннем устройстве игры и скрипты Ghidra для их получения |
| `tools/gametest/` | автоматизация тестов: два окна игры, клики, скриншоты, проверенные сценарии |
| `mods/CoopTest/` | первый прототип на Mod API (заморожен) |

## Правовое

Репозиторий приватный: в `decompiled/code`, `decompiled/data` и `decompiled/symbols` лежит декомпиляция
игры (код HopFrog), публиковать её нельзя. Остальное — наш код и заметки.
Forager © HopFrog / Humble Games. Проект не связан с авторами игры.
MinHook — BSD-2-Clause.
