# ADR 0036: Wayland-клиент напрямую, без libwayland

Дата: 2026-11-07. Этап: 34.

## Контекст

Окно до этого этапа умел открывать только X11-клиент. На Wayland-сессии
(KDE Plasma 6, GNOME 45+, Sway) он работает лишь при наличии XWayland: сам
XWayland — это ещё один сервер между приложением и композитором, и без него
(или когда он отключён) окна нет вовсе. Пользовательская машина — CachyOS с
KDE Plasma 6 на Wayland, поэтому «нативный Wayland» перестал быть
теоретическим требованием.

Инвариант проекта: **только стандартная библиотека C++20**. Это исключает
`libwayland-client`, `wayland-protocols` (генератор кода) и `libxkbcommon`.

## Решение

### 1. Протокол реализован руками, как и X11

Второй бэкенд — это ещё один «провод»: `AF_UNIX`-сокет, сообщения с заголовком
`object id | size << 16 | opcode`, аргументы little-endian с выравниванием до 4
байт. Кодирование вынесено в `proto::*` (как `proto::*` у X11), поэтому его можно
тестировать без композитора: `message()`, `string_arg()`, `bind_message()`,
`create_buffer_message()`, `decode_header()`, `Reader`, `decode_configure()`,
`keysym_for_evdev()`.

Из протокола реализовано ровно то, что нужно окну браузера:

| объект | запросы/события |
|---|---|
| `wl_display` | `get_registry`, `sync`, `error` |
| `wl_registry` | `bind`, `global`, `global_remove` (игнорируется) |
| `wl_compositor` | `create_surface` |
| `wl_shm`, `wl_shm_pool`, `wl_buffer` | `create_pool` (fd через `SCM_RIGHTS`), `create_buffer`, `release`, `destroy` |
| `xdg_wm_base` | `get_xdg_surface`, `pong`, `ping` |
| `xdg_surface` | `get_toplevel`, `ack_configure`, `configure` |
| `xdg_toplevel` | `set_title`, `set_app_id`, `configure`, `close` |
| `wl_seat` | `get_pointer`, `get_keyboard`, `capabilities` |
| `wl_pointer` | `enter`, `motion`, `button`, `axis` |
| `wl_keyboard` | `key` |
| `zxdg_decoration_manager_v1` | `get_toplevel_decoration`, `set_mode(server_side)` |

Версия каждого глобального объекта запрашивается как `min(версия сервера, 1)`:
лишние события не нужны, а v1 есть у всех композиторов.

### 2. Кадры — `wl_shm`, `XRGB8888`, двойная буферизация

`wl_shm` был выбран вместо `wl_egl`/`wl_drm`: браузер рисует программно на CPU,
GPU-контекст не нужен, а `wl_shm` — единственный путь без `libEGL` и без
DRM-узла. Буфер — `XRGB8888` (альфа не используется: страница всегда
непрозрачная, а premultiplied-ARGB потребовал бы умножения каждого пикселя).

Файл разделяемой памяти создаётся `memfd_create`; вызов идёт через `syscall`,
потому что при `-std=c++20` (без `_GNU_SOURCE`) glibc не объявляет обёртку.
Дескриптор передаётся управляющим сообщением `SCM_RIGHTS` через `sendmsg` —
это единственное место протокола, где нужны file descriptors, и первым байтом
запроса `create_pool`.

Два слота на кадр: `present()` чередует их и не блокируется, если композитор ещё
держит предыдущий буфер (`wl_buffer.release` снимает флаг). Кадр уходит сразу,
без `wl_surface.frame` — вертикальную синхронизацию делает композитор.

### 3. Клавиатура — таблица вместо libxkbcommon

На X11 раскладку отдаёт сервер (`GetKeyboardMapping`). В Wayland композитор
присылает xkb-файл, и его разбор — это libxkbcommon, то есть внешняя
зависимость. Поэтому в бэкенде лежит таблица `evdev code → keysym` для
US-раскладки: буквы, цифры, стрелки, навигация, `Esc`/`Tab`/`Enter`/`Space`. Для
окна просмотра (прокрутка, клики) этого достаточно; полноценные раскладки и
модификаторы — отдельная задача, которую можно решить своим разбором xkb, не
нарушая инвариант.

### 4. Один цикл событий на два бэкенда

Интерфейсы `x11::Window` и `wayland::Window` совпадают по форме (`open`,
`present`, `poll`, `Event` с теми же полями), поэтому в `main.cpp` появились
traits-классы (`X11Backend`, `WaylandBackend`) и шаблон
`run_window_loop<Backend>`. Обработка прокрутки/кликов/клавиш написана один раз.
Выбор бэкенда — `--backend=auto|x11|wayland`; `auto` предпочитает Wayland, потому
что в Wayland-сессии XWayland может быть просто не установлен.

## Последствия

* `bonk view` работает на чистом Wayland без XWayland; `--backend=x11`
  остаётся для X11 и для отладки.
* Появился **второй** потребитель `paint::Canvas` и общий шаблон цикла — третьего
  бэкенда (например, `wl_egl` или фреймбуфер) можно добавить, не трогая логику.
* Мок-композитор в `tests/shell/wayland_test.cpp` — это фактически эталон
  протокола: он проверяет порядок `get_registry → bind → sync`, `ack_configure`,
  `SCM_RIGHTS` и разбор событий ввода без дисплея.
* Ограничения (нет CSD, `frame`, `wl_region`, буфера обмена, non-US раскладок)
  задокументированы в `docs/STAGE-34.md` и не влияют на конформанс-метрики.
