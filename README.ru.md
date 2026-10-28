<p align="center">
  <img src="docs/images/logo.png" alt="Forager Co-op Mod" width="560">
</p>

<p align="center">
  <b>Сетевой кооператив для <a href="https://store.steampowered.com/app/751780/Forager/">Forager</a> — до 4 игроков, одна DLL, файлы игры не меняются.</b>
</p>

<p align="center">
  <a href="https://github.com/melogo5/forager-coop/releases/latest"><img src="https://img.shields.io/github/v/release/melogo5/forager-coop?include_prereleases&label=download" alt="Скачать"></a>
  <a href="https://discord.gg/BjsubWefm4"><img src="https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="Лицензия MIT"></a>
</p>

<p align="center">
  <a href="README.md">English</a> · Русский
</p>

## Видео

<p align="center">
  <a href="https://www.youtube.com/watch?v=apy2HmzMUoY"><img src="docs/images/showcase.jpg" alt="Смотреть обзор Forager Co-op Mod на YouTube" width="720"></a>
  <br>
  <a href="https://www.youtube.com/watch?v=apy2HmzMUoY"><b>▶ Смотреть видео на YouTube</b></a>
</p>

## Возможности

- **До 4 игроков** в одном мире: хост и до трёх гостей.
- **Приглашения через Steam** — хост нажимает **F2** в мире и приглашает друзей; IP и VPN не нужны.
  Работает и по LAN, Hamachi, Radmin.
- **Пункт COOP в главном меню** — создать мир или подключиться к серверу из списка в вашей сети.
- **Общий мир:** ресурсы, мобы, постройки, сундуки, хранилища, мосты, острова (покупает хост), время суток и
  погода.
- **Общий прогресс:** один уровень и опыт, общие монеты, одно дерево навыков (навык, изученный кем угодно, изучен у
  всех; очки навыков у каждого свои), общий музей и квесты NPC.
- **Свой персонаж:** у каждого гостя свои инвентарь, снаряжение, спутники и костюм — хост хранит их под именем
  гостя, при повторном входе всё на месте.
- **Подземелья вместе:** все входят и выходят вместе; головоломки, фонари, ключи и сундуки синхронизируются.
- **Без game over:** погибший игрок возрождается на стартовом острове (или у входа в подземелье).

> **Статус: бета.** Большая часть игры в коопе работает, но рассинхроны ещё бывают.
> Об ошибках пишите в [Discord](https://discord.gg/BjsubWefm4) или в [Issues](https://github.com/melogo5/forager-coop/issues).

## Требования

- У каждого игрока своя копия **Forager в Steam, версия 4.1.9** (текущий билд Steam, 2021-01-29).
  На другом билде DLL ничего не делает.
- Windows.
- В коопе играется только режим **Classic**.
- Моды игры (Mod API) не поддерживаются.

## Установка

1. Скачайте `version.dll` из [последнего релиза](https://github.com/melogo5/forager-coop/releases/latest).
2. Положите в папку игры, рядом с `Forager.exe`
   (Steam → Forager → Управление → Просмотреть локальные файлы).
3. Это нужно сделать каждому игроку.

В главном меню появится пункт **COOP**. Удаление — стереть `version.dll` из папки игры.

## Как играть

**Хост**

1. Главное меню → **COOP** → **CREATE** → выбрать сейв или **+** (новый мир Classic).
2. В мире нажать **F2**, чтобы пригласить друзей через Steam.

**Гость**

- **Через Steam:** принять приглашение хоста в Steam — игра подключится сама (если Forager не запущен, Steam его
  запустит).
- **В одной сети (LAN, Hamachi, Radmin):** главное меню → **COOP** → ввести имя в поле **NAME** → выбрать сервер
  хоста → **JOIN**. Если сервера нет в списке — ввести IP хоста в поле **IP** → **CONNECT**.

Мир хоста загрузится у гостя сам; собственные сейвы гостя не затрагиваются.

**Полезно знать**

- Брандмауэр Windows при первом запуске спросит разрешение для Forager — его нужно дать. Для игры по LAN/VPN
  нужны порты TCP 7790 и UDP 7791.
- Острова покупает только хост (вкладка BUY LAND), и достижения Steam получает только он.
- Выход — **Save and exit to menu**. Когда выходит хост, гости возвращаются в главное меню. Гость может зайти снова
  без перезапуска игры.
- Пустой файл `coop_skip_intro.txt` рядом с `Forager.exe` отключает заставки издателей при запуске.

## Известные проблемы

- Испытание **Void** в коопе пока не поддерживается.
- Изредка гость загружает немного другой мир (лишние или недостающие деревья); мод исправляет это за несколько
  секунд.
- Если что-то сломалось — лог лежит в папке игры: `coop_native_<число>.log`. Приложите его к сообщению об ошибке.

## Сборка из исходников

Нужны Visual Studio Build Tools с C++ (toolset x86).

```powershell
git clone --recursive https://github.com/melogo5/forager-coop.git
cd forager-coop
powershell -File native\build.ps1 -Install -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Forager"
```

DLL собирается в `native\build\version.dll`; `-Install` дополнительно копирует её в папку игры.
Пуш тега `v*` собирает DLL в GitHub Actions и публикует релиз (тег с дефисом, например `v0.2.0-beta1`, —
pre-release).

### Как это устроено

Forager собран GameMaker YYC (GML скомпилирован в машинный код), поэтому скриптов для правки нет. Вместо этого
`version.dll` проксирует одноимённую системную библиотеку и ставит хуки
([MinHook](https://github.com/TsudaKageyu/minhook)) на функции игры. Хост авторитетен: мир живёт и сохраняется
только у хоста; гость получает его снимок при подключении и дальше — поток изменений. Другие игроки — вторые
`objPlayer` с отключённой логикой («марионетки»); действия гостя хост исполняет от его имени.

Документация для разработчиков:

| Папка | Что внутри |
|---|---|
| [`native/`](native/) | исходники `version.dll` |
| [`docs/coop/`](docs/coop/) | спецификация, архитектура, статус, разборы плейтестов |
| [`tools/gametest/`](tools/gametest/) | автоматизация тестов: два окна игры, клики, скриншоты |
| [`mods/CoopTest/`](mods/CoopTest/) | первый прототип на Mod API (заморожен) |

## Правовое

Код и заметки в репозитории — под [лицензией MIT](LICENSE). Кода и ресурсов игры в репозитории нет.
Для игры нужна купленная копия Forager.

Forager © HopFrog / Humble Games. Проект не связан с авторами игры и не одобрен ими.
MinHook — BSD-2-Clause.
