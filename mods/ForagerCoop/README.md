# Forager Coop — пакет для Steam Workshop

Мод Mod API (`ForagerCoop`), который сам ставит кооп без копирования `version.dll` в папку игры. Основан на
Workshop-интеграции для v0.1.2 (MIT).

## Сборка

Релизный workflow собирает пакет вместе с `version.dll` и прикладывает `ForagerCoop-Workshop-<тег>.zip` к релизу.
Локально:

```powershell
powershell -File native\build.ps1
powershell -File mods\ForagerCoop\build.ps1 -Version v0.1.3   # -> mods\ForagerCoop\build\ForagerCoop-Workshop-v0.1.3.zip
```

## Загрузка в Steam

Распаковать архив в `%LOCALAPPDATA%\Forager\mods\` (получится `mods\ForagerCoop\`) и загрузить мод из меню
MODDING игры.

## Как работает

- `Main.gml` (генерируется `package.py` из `Main.gml.in`) содержит обе DLL. После COMPILE в MODDING он пишет
  в `%LOCALAPPDATA%\Forager`: `ForagerCoop-<хеш>.dll` (кооп этой сборки), `catch_error.dll` (мост `bridge.c`,
  только один раз), `ForagerCoop-current.txt` (имя DLL для моста) и метку `ForagerCoop-<хеш>.ready`.
- При следующем запуске игра загружает `catch_error.dll` из профиля. Мост передаёт вызовы оригинальной
  `catch_error.dll` игры и загружает DLL из `ForagerCoop-current.txt`, если мод есть в `mods\ForagerCoop` или
  в подписках Workshop и рядом с `Forager.exe` нет `version.dll` (ручная установка важнее).
- Новая версия мода пишет DLL под новым именем (загруженная старая заблокирована) и переключает
  `ForagerCoop-current.txt`; нужен ещё один перезапуск игры. Старые `ForagerCoop-*.dll` остаются в профиле.
- Мост загружен всё время, пока работает игра, поэтому заменить его новым мостом нельзя: при изменении
  `bridge.c` нужно новое имя метки `ForagerCoop-bridge-2.ready` и способ обновления.

Отключить: создать `ForagerCoop-disabled.txt` в `%LOCALAPPDATA%\Forager`. Удалить полностью: стереть там
`catch_error.dll`, `ForagerCoop-*.dll`, `ForagerCoop-*.ready`, `ForagerCoop-current.txt`.
