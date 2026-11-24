# Предпросмотр интерфейса Ovygram без аккаунта

Отдельная сборка Linux открывает нативные Qt-виджеты Telegram, те же карточки
и диалоги установки, которые использует Ovygram. Профили и данные придуманы
для проверки. Скриншот можно получить в облаке без входа в Telegram и без
подключения к личному компьютеру.

## Первый запуск в Codex Cloud

Выполните команды в корне репозитория на Debian 13 x86-64 с компилятором C++20,
CPython 3.13, `uv`, `pkg-config` и `bubblewrap`.

```sh
tools/ovygram/ui-preview --setup
tools/ovygram/ui-preview --build-only
```

Подготовка скачивает инструменты и Qt 6.11 в `out/ui-preview-tools` от имени
текущего пользователя. Сборка кэшируется в `out/ui-preview`. Оба пути можно
переопределить через `OVYGRAM_UI_PREVIEW_DEPS` и
`OVYGRAM_UI_PREVIEW_BUILD_DIR`. Число рабочих процессов задаёт
`OVYGRAM_UI_PREVIEW_JOBS`. Для другой Linux-среды установите CMake 3.25–3.31,
Ninja, Qt 6.11 с заголовками CorePrivate и GuiPrivate и модулем Qt Image Formats, Boost, GLib,
GObject Introspection, Pango, KCoreAddons, OpenSSL и заголовки JPEG, LZ4,
xxHash, Vulkan, OpenGL и zlib. Qt можно указать через `CMAKE_PREFIX_PATH`.
Используются закреплённые подмодули репозитория.

## Проверка окон клиента

Для нового чата есть [готовый промпт для задач по интерфейсу](ui-design-prompt.txt).
Он подходит и для окна клиента, и для SDK-плагина.

```sh
tools/ovygram/ui-preview --scenario list-before --height 1000 --screenshot /tmp/plugin-list-before.png
tools/ovygram/ui-preview --scenario list-after --height 1000 --screenshot /tmp/plugin-list-after.png
tools/ovygram/ui-preview --scenario list-after --check
tools/ovygram/ui-preview --scenario details --check --screenshot /tmp/plugin-details.png
tools/ovygram/ui-preview --scenario install-new --check --screenshot /tmp/install-new.png
tools/ovygram/ui-preview --scenario install-update --check --screenshot /tmp/install-update.png
tools/ovygram/ui-preview --scenario install-multiple --check
tools/ovygram/ui-preview --scenario install-long --check
tools/ovygram/ui-preview --scenario long --check
tools/ovygram/ui-preview --scenario proxies --check
tools/ovygram/ui-preview --scenario install-update --theme light --scale 125 \
  --width 1000 --height 760 --click 'Подробности файла' --screenshot /tmp/file-details.png
```

`list-before` показывает текущие карточки списка. `list-after` показывает
предложение компактного списка с теми же тестовыми плагинами.

С `DISPLAY` или `WAYLAND_DISPLAY` команда без `--check`, `--click` и
`--screenshot` оставляет окно открытым. В headless-среде
`--screenshot` сохраняет PNG с того же Qt-рендера. По умолчанию применяется
тема Telegram Night, для светлой темы используйте `--theme light`.
`--width` и `--height` задают доступную область окна.

При каждой правке окна запускайте нужный сценарий для тем и размеров,
которых касается изменение. Проверьте связанные действия, раскройте
сворачиваемые строки, проверьте перенос и прокрутку на длинном тексте.
Приложите скриншоты в итоговом сообщении. Для нового окна добавьте сценарий
в `tools/ovygram/ui_preview/main.cpp` и вызывайте тот же построитель виджетов
из клиента и предпросмотра.

## Проверка интерфейса плагина

```sh
tools/ovygram/ui-preview --plugin tools/ovygram/examples/section.ovyplugin \
  --menu 'Раздел плагина' --click 'Нажать' --screenshot /tmp/plugin-section.png
tools/ovygram/ui-preview --plugin tools/ovygram/examples/note_draft.ovyplugin \
  --setting open_draft --click 'Сохранить' --screenshot /tmp/note-saved.png
tools/ovygram/ui-preview --plugin /abs/path/plugin.ovyplugin --screenshot /tmp/plugin-settings.png
tools/ovygram/ui-preview --plugin /abs/path/plugin.ovyzip --setting open_settings \
  --screenshot /tmp/plugin-window.png
```

Без `--menu` и `--setting` открывается поверхность, которую плагин показывает
при запуске, или карточка с его декларативными настройками. `--menu` вызывает
пункт главного меню по подписи, а `--setting` — действие `Button` или `Link`
по ключу. Можно открыть `Section`, `Dialog` и `Window` и проверить их
локальные обработчики. `Section` в предпросмотре показан в отдельной панели.
Для способов открытия, связанных с реальным чатом, добавьте вызов того же
построителя из тестового пункта главного меню.

Плагин выполняется в обычном изолированном хосте без сети, с временным
профилем и вымышленным аккаунтом. Для плагина с зависимостями передайте
`--dependencies /abs/path/site-packages` с заранее подготовленными
совместимыми колёсами (например, каталогом от `pip install --target`).
Элементы, которые требуют живой переписки, серверных запросов или
настоящих системных окон Windows, проверяются отдельным сценарием клиента.
