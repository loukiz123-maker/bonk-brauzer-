# Этап 34: свой Wayland-бэкенд

До этого этапа окно умел открывать только X11-клиент (`docs/STAGE-03.md`): он
подключается к `/tmp/.X11-unix/XN`, читает MIT-MAGIC-COOKIE-1 из `~/.Xauthority`
и рисует через `PutImage`. На X11-сессии этого достаточно. На **Wayland-сессии**
(KDE Plasma 6, GNOME, Sway) `$DISPLAY` появляется только если установлен
XWayland, а сам XWayland — лишний сервер в цепочке: приложение → XWayland →
композитор. Если XWayland нет (или он отключён), окна нет вовсе.

Поэтому этап добавляет второй, нативный бэкенд — **свой клиент протокола
Wayland**, без libwayland, без libxkbcommon, без SDL. Это тот же принцип, что и
у X11-клиента: протокол реализован вручную по `wayland.xml`.

## Результат в цифрах

| набор | этап 33 | этап 34 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 586 | **594** | +8 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tokenizer / tree | 6 792 / 6 820 · 1 792 / 1 792 | то же | без изменений |
| test262 (в конверте) | 10 309 / 18 391 (56.1 %) | то же | без изменений |

Этап платформенный: JS-движок не трогали, поэтому конформанс-цифры те же. Новые
8 тестов — `tests/shell/wayland_test.cpp`.

## Что сделано

### 1. Транспорт и рукопожатие

`Window::open` разрешает сокет (`$WAYLAND_DISPLAY` относительно
`$XDG_RUNTIME_DIR`, либо абсолютный путь — так тесты подключаются к мок-серверу),
подключается `AF_UNIX`/`SOCK_STREAM` и делает то же, что делает libwayland:

```
wl_display.get_registry(new_id)      → список глобальных объектов
wl_registry.bind(name, interface)    → wl_compositor, wl_shm, xdg_wm_base,
                                       wl_seat, zxdg_decoration_manager_v1
wl_display.sync(new_id)              → wl_callback.done (roundtrip)
```

Версия каждого глобального объекта запрашивается как `min(версия сервера, 1)`:
нам хватает запросов v1, а более высокая версия — это больше событий, которые
пришлось бы разбирать. Если композитор не отдал `wl_compositor`, `wl_shm` или
`xdg_wm_base`, `open()` возвращает ошибку с внятным текстом, а не падает.

Каждое сообщение кодируется вручную: `object id` (u32), затем
`size << 16 | opcode`, затем аргументы; строки — с завершающим NUL и выравниванием
до 4 байт. Чтение («`proto::Reader`») не выходит за границы тела сообщения: на
битом сообщении разборщик возвращает `false`, а не читает мусор.

### 2. Окно: xdg-shell

```
wl_compositor.create_surface
xdg_wm_base.get_xdg_surface   → xdg_surface
xdg_surface.get_toplevel      → xdg_toplevel (+ set_title, set_app_id)
zxdg_decoration_manager_v1.get_toplevel_decoration(toplevel).set_mode(server_side)
wl_surface.commit             → композитор присылает configure
xdg_surface.ack_configure(serial); xdg_toplevel.configure(w, h, states)
```

Пока не пришёл первый `configure`, рисовать нельзя — `open()` ждёт его
(с ограничением по числу итераций, а не по бесконечному циклу). Размер из
`xdg_toplevel.configure` побеждает запрошенный в командной строке: это решает
композитор, а не приложение.

`zxdg_decoration_manager_v1` — необязательная часть (KWin и Mutter её
поддерживают): без запроса `server_side` у окна на KDE **нет заголовка и кнопки
закрытия**. Если глобального объекта нет, окно просто останется без рамки, а
закрыть его можно через `Alt+F4` (композитор пришлёт `xdg_toplevel.close`).

### 3. Кадры: wl_shm с двойной буферизацией

Кадр — это `XRGB8888`-буфер в разделяемой памяти:

* файл создаётся `memfd_create` (через `syscall`, чтобы не зависеть от
  `_GNU_SOURCE`), с запасным вариантом «временный файл в `$XDG_RUNTIME_DIR`»;
* `mmap` даёт два слота по `stride * height` — двойная буферизация;
* дескриптор уходит композитору **управляющим сообщением `SCM_RIGHTS`**
  (`sendmsg`), а не байтами в теле запроса — единственное место в протоколе, где
  нужны file descriptors;
* `present()` копирует канвас (RGBA8) в слот и отправляет
  `wl_surface.attach` + `damage` + `commit`.

Слот выбирается по флагу `wl_buffer.release`: если композитор ещё читает
предыдущий буфер, переиспользуем текущий, а не блокируем цикл событий.

### 4. Ввод: мышь и клавиатура

`wl_seat.capabilities` включает `wl_pointer` и `wl_keyboard`; события
переводятся в ту же модель, что у X11-бэкенда (`EventType::Motion`,
`ButtonPress`, `KeyPress`, `Close`):

* `wl_pointer.enter/motion` — координаты в 24.8 fixed-point (`proto::Reader::fixed`);
* `wl_pointer.button` — `BTN_LEFT/BTN_RIGHT/BTN_MIDDLE` → 1/3/2;
* `wl_pointer.axis` — вертикальная ось в колесо (знак < 0 → «вверх»);
* `wl_keyboard.key` — evdev-код → keysym.

Вот здесь пришлось отказаться от привычного пути: на X11 раскладку отдаёт сервер
(`GetKeyboardMapping`), а в Wayland композитор присылает **xkb-файл**, для
разбора которого нужен libxkbcommon. Внешних зависимостей у проекта нет, поэтому
в бэкенде лежит таблица evdev → keysym для US-раскладки (буквы, цифры, стрелки,
`Home`/`End`/`Page Up/Down`, `Esc`, `Tab`, `Enter`, `Space`, `Backspace`,
`Delete`). Чего в ней нет — того нет: не-US раскладки, модификаторы, `Shift`.
Это осознанное ограничение, а не забытая ветка; окно прокручивается стрелками и
`Page Up/Down`, и этого достаточно для просмотра страниц.

### 5. Выбор бэкенда

`bonk view` получил `--backend=auto|x11|wayland` (по умолчанию `auto`):
Wayland, если есть `$WAYLAND_DISPLAY` и сокет, иначе X11/XWayland. Если нет ни
того, ни другого — сообщение с обоими вариантами и подсказка про
`--screenshot`. Неверное значение — usage-ошибка (код выхода 2).

Цикл событий в `main.cpp` теперь один на оба бэкенда: два маленьких traits-класса
(`X11Backend`, `WaylandBackend`) и шаблон `run_window_loop<Backend>`, поэтому
прокрутка, клики и клавиши не дублируются.

## Как проверить

```console
$ make -j && ./build/bonk_test --filter=wayland     # 8 тестов, без дисплея
$ bonk view tests/fixtures/sample.html              # Wayland-окно на KDE/GNOME/Sway
$ bonk view tests/fixtures/sample.html --backend=x11   # через XWayland
$ bonk view tests/fixtures/sample.html --screenshot out.png   # headless, как раньше
```

Тест `wayland_window.handshake_present_and_input` поднимает **мок-композитор**
(сокет в `/tmp`, отдельный поток), который отвечает на `get_registry`, `bind`,
`sync`, присылает `configure`, `capabilities`, движение мыши, нажатие, `Left` и
`close`. Клиент проходит полный путь: рукопожатие → создание буфера → `attach` /
`commit` → разбор событий. Это единственный способ проверить протокол в песочнице
без композитора — и он проверяет именно то, что ломается чаще всего: порядок
запросов, выравнивание аргументов и `ack_configure`.

## Открыто

* нет client-side decorations и `xdg-decoration` v2 — полагаемся на серверные
  рамки или на `Alt+F4`;
* нет `frame`-колбэков (`wl_surface.frame`): кадр отправляется сразу, без
  синхронизации с вертикальной синхронизацией композитора;
* нет `wl_region` (opaque/input regions), `wl_data_device` (буфер обмена),
  `wl_touch`, `text-input`, дробного масштабирования и `wp_fractional_scale`;
* раскладка клавиатуры — US-таблица, а не xkb;
* при `configure` другого размера страница продолжает рендериться в исходный
  вьюпорт (как и в X11-бэкенде); ресайз с пере-лейаутом — отдельный этап.
