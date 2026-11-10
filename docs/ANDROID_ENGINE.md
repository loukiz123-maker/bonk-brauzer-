# Android-движок: архитектура и проверяемые этапы

**Статус:** по запросу переключаем целевой Linux gaming-runtime с Android SDK Emulator на Waydroid — контейнерный Android с прямым доступом к host hardware, а не копию закрытого BlueStacks. Добавлены host-side Waydroid adapter и CLI-лаунчер `bonk waydroid`: read-only status, подтверждённый старт сессии, открытие full UI, установка выбранного APK и запуск точного package ID. Также остаются generic ADB и SDK Emulator APIs. Проверено сборкой, unit-тестами и synthetic CLI end-to-end shim; Waydroid, Wayland compositor, Binder, GPU acceleration и настоящие игры в этой среде отсутствуют. Поддержка «любых игр», game-control mapping и интегрированный видеоканал не заявляются. Архитектурный draft LynxDroid и host acceptance gates: [docs/LYNXDROID_ARCHITECTURE.md](LYNXDROID_ARCHITECTURE.md).

## Исходное состояние репозитория

Текущий checkout — C++20-проект **Bonk Brauzer**, а не отдельный репозиторий LynxDroid. В нём есть HTML/CSS/JS-движок и X11-оболочка, но нет установленного/поставляемого Android runtime, Waydroid image, scrcpy, GUI-игрового лаунчера, APK-раскладок или кода редактора управления. Добавлены host-side adapter и CLI-лаунчер к внешнему Waydroid; Android UI пока не встраивается в окно Bonk.

Сборка проекта подхватывает новые `src/*/*.cpp` и `tests/*/*.cpp` автоматически. Проверенный до изменений базовый результат: `make -j2 verify` — 271 тест, ошибок нет.

## Архитектурное решение

### Целевой runtime-кандидат: Waydroid на Linux

После уточнения цели (игровой Android player на Linux) **Android SDK Emulator/AVD больше не считается основным gaming runtime**. Первый кандидат — пользовательская установка [Waydroid](https://github.com/waydroid/waydroid): upstream описывает его как Android в Linux-контейнере с доступом Android-системы к host hardware. Это ближе к производительности нативного Linux runtime, чем developer-oriented AVD, но это **не технология BlueStacks и не гарантия совместимости игр**.

BlueStacks — закрытый продукт; его внутренний runtime нельзя переиспользовать или скопировать из этого checkout. В проекте реализован отдельный adapter и CLI-лаунчер `bonk waydroid` для установленного пользователем Waydroid: read-only `waydroid status`, foreground `waydroid session start`, `waydroid show-full-ui`, `waydroid app install` и `waydroid app launch`. Adapter не скачивает/инициализирует Android images, не запускает `sudo`, не загружает kernel modules и не меняет системные права.

Waydroid требует подходящий Linux-хост с настроенным Wayland session/D-Bus, Binder support и доступом к GPU render device. `session start` использует окружение desktop-пользователя; текущая оболочка Bonk — X11 и не встраивает Wayland surface. `show-full-ui` открывает UI самого Waydroid, а не выводит кадры внутрь окна Bonk. Runtime/process start не является Android boot/readiness сигналом.

### CLI-лаунчер

- `bonk waydroid [--waydroid /absolute/path]` только читает и печатает session/container status; он не стартует runtime.
- `bonk waydroid --confirm-start` явно разрешает старт остановленной сессии, ждёт до 120 секунд, пока `waydroid status` покажет running и foreground-процесс останется жив, затем открывает Waydroid full UI. Если сессия уже запущена, Bonk использует её без принятия владения.
- `--show-ui` открывает UI существующей сессии; `--install-apk /absolute/file.apk --confirm-install` устанавливает только явно выбранный файл; `--launch-package com.example.game` запускает ровно переданный package ID. При любом из этих действий full UI открывается автоматически.
- Если сессия остановлена, действия без `--confirm-start` завершаются ошибкой и ничего не стартуют. Установка требует отдельного `--confirm-install` для этого APK.
- Когда Bonk сам стартовал foreground `session start`, процесс лаунчера остаётся активным и супервизирует его до естественного выхода либо Ctrl-C/SIGTERM. При завершении Bonk останавливает только принадлежащую ему process group; Waydroid может остановить общий host container. Для уже работающей внешней сессии Bonk не запускает stop и не удерживает её процесс.
- CLI не является отдельным графическим интерфейсом, не отображает каталог установленных игр и не встраивает Waydroid surface в X11 окно Bonk.

«Любые игры» технически обещать нельзя: совместимость зависит от ABI и ARM translation, Vulkan/GPU, Google Play Services, Play Integrity, DRM и anti-cheat; отдельные игры могут блокировать контейнер или виртуальное устройство. Первый этап — проверка выбранной игры на целевом железе, а не обещание каталога BlueStacks. Android SDK Emulator остаётся необязательным тестовым backend; Cuttlefish также не выбран как gaming runtime.

### Границы модулей

```text
Bonk Browser (текущая desktop оболочка — X11; Waydroid surface не встраивается)
  └─ host-side Android adapter (IPC лаунчера ещё нет)
       ├─ EngineSession owns `waydroid session start` foreground child
       ├─ Waydroid CLI: status / confirmed session start / install / launch / full UI
       └─ generic ADB helpers остаются для диагностики и SDK Emulator backend
              │
              ▼
Пользовательский Waydroid system service + Android container
  ├─ host Wayland session + Linux Binder/GPU devices
  └─ собственное Android UI окно (не видеоканал Bonk)
```

`EngineSession::start()` означает только, что ОС создала процесс. `start_waydroid_session()` дополнительно проверяет `waydroid status`, отказывается брать уже запущенную сессию и требует подтверждения, потому что запуск/остановка session может затронуть общий Waydroid container. `ProcessState::Running` **не** означает, что Android загрузился или игра готова. Вызовы `list_adb_devices()`, `query_guest_status()`, `wait_for_guest_boot()`, `wait_for_runtime_and_guest_boot()`, `capture_screenshot_rgba()`, `send_tap_normalized()`, `send_swipe_normalized()`, `send_android_key()`, `query_installed_package_paths()` и `launch_installed_package()` остаются отдельными generic ADB-командами и требуют явного ADB-server consent. Waydroid native UI запускается отдельно; кадры и game-control mapping в Bonk пока отсутствуют.

## Контракт лаунчера

Ниже перечислены текущие реализации и целевые требования; статус каждой строки указан явно.

| Направление | Контракт |
|---|---|
| Запуск | Linux gaming pilot использует установленный пользователем Waydroid и текущий Wayland/D-Bus environment; SDK Emulator/AVD остаётся опциональным тестовым backend. `start_waydroid_session()` не инициализирует images и требует отдельного подтверждения. Ответ содержит отдельные `runtime_state` и `guest_state`. |
| Остановка | `stop(grace_period)`; сначала штатное завершение, затем явная эскалация. Остановка затрагивает только процессную группу, созданную движком. |
| Состояние | `Stopped`, `Starting`, `RuntimeRunning`, `GuestBooting`, `Ready`, `Stopping`, `Failed`; поля runtime и guest нельзя смешивать. В текущем коде доступны состояния процесса `stopped/running/exited/failed`; `wait_for_guest_boot()` опрашивает ADB до `sys.boot_completed=1`, а `wait_for_runtime_and_guest_boot()` дополнительно обнаруживает завершение принадлежащего `EngineSession` runtime во время ожидания. Ни один вызов не означает готовность игры и не заменяет непрерывный монитор состояния. |
| Кадры | Сейчас `capture_screenshot_rgba()` возвращает один декодированный PNG RGBA без sequence/timestamp/rotation; лимит 32 MiB сжатого PNG и 16 777 216 пикселей. Это не видеопоток и не гарантия низкой задержки. Будущий IPC/frame API должен добавить stride, sequence, monotonic timestamp и rotation. |
| Touch | Есть одиночный tap и прямолинейный swipe через `send_swipe_normalized()`: обе пары координат нормализованы в `[0,1]` и округляются в пиксели `[0,width-1] × [0,height-1]`; swipe длится 1 ms..30 s. ADB shell просит Android синтезировать жест, но доставка Down/Move/Up в приложение не подтверждена live-тестом. Нет caller-controlled событий, произвольной траектории, pointer ID, отмены, multitouch или joystick; letterbox/rotation учитывает вызывающий UI. |
| Клавиши | Сейчас только одиночный `adb shell input keyevent <numeric-code>` (1..65535); KeyDown/KeyUp, repeat и удержание не реализованы. Не передавать сырые клавиши хоста как Android-коды без явного преобразования. |
| Запуск приложения | `launch_installed_package()` запрашивает MAIN/LAUNCHER activity уже установленного приложения по подтверждённому package ID; эта функция APK не устанавливает. Успешный exit `am start` не доказывает готовность приложения/игры. |

Когда лаунчер и движок будут разнесены по процессам, предпочтителен версионированный **Unix-domain socket** в приватном runtime-каталоге: управляющие сообщения отдельно от кадров, кадры — бинарные с длиной и метаданными, без base64 в JSON. Не открывать неаутентифицированный TCP-порт. Если UI работает в браузерном preview, браузерный код не должен обращаться к `localhost`; нужен native IPC или same-origin proxy, принадлежащий приложению.

### Блокер интеграции со старым Android-агентом

По предоставленному QA-отчёту, старый `TouchAgentService` создаёт `LocalServerSocket(String)` в абстрактном namespace Android, тогда как Python-клиент подключается к файловому pathname. Эти адресации несовместимы; C++-модуль пока не содержит IPC-клиента и не должен подключаться к агенту, пока стороны не согласуют namespace/address, проверку клиента и версию протокола. Нужен явный handshake и проверка peer/прав до передачи команд. **Решение на текущий этап: интеграцию отложить**; C++ host-side API остаётся самостоятельным до появления исходников агента, согласованного IPC-протокола, установленного Waydroid/target host и тестового APK для совместного smoke test. Источники из отчёта: [TouchAgentService.java](https://github.com/loukiz123-maker/bonk-brauzer-/blob/8d13e22/android_agent/app/src/main/java/id/lynxdroid/agent/TouchAgentService.java#L88-L118) и [Android LocalServerSocket API](https://developer.android.com/reference/android/net/LocalServerSocket).

Реализованы generic EngineSession, optional SDK Emulator/AVD command builder, Waydroid CLI adapter и CLI-команда `bonk waydroid` (status, explicit session start, selected APK install, exact package launch и full UI), ADB guest-state query и bounded boot wait. IPC-сокет, непрерывный guest monitor, Bonk-integrated frame surface, low-latency video и BlueStacks-style game-control mapping остаются незавершёнными; synthetic CLI flow проверен fake Waydroid process, настоящий Waydroid host и игра не тестировались.

## Этап 1: выполненная работа

Новые файлы:

- `include/bonk/android/engine_session.hpp` — типы `RuntimeCommand`, `EmulatorConfig`, `ProcessState`, `SessionStatus` и класс `EngineSession`.
- `src/android_engine/engine_session.cpp` — `posix_spawn` с явным абсолютным путём (без shell/PATH-поиска), отдельная process group, non-blocking `poll`, `SIGTERM` → `SIGKILL`, сбор exit code/signal и очистка дочернего процесса в деструкторе.
- `tests/android_engine/engine_session_test.cpp` — тесты командной строки, ошибок отсутствующего executable, повторного запуска, штатного выхода, NUL в аргументах и остановки тестового `/bin/sleep`.
- `include/bonk/android/waydroid_runtime.hpp` / `src/android_engine/waydroid_runtime.cpp` — абсолютнопутевые argv builders, bounded parser/query `waydroid status`, подтверждённый старт foreground Waydroid session, install выбранного APK, запуск package ID и `show-full-ui`.
- `tests/android_engine/waydroid_runtime_test.cpp` — Fake Waydroid CLI проверяет status, consent, владение сессией, ожидание состояния и argv операций.
- `include/bonk/cli/cli.hpp`, `src/cli/cli.cpp`, `src/apps/main.cpp` — команда `bonk waydroid`: status-only по умолчанию, явный consent на старт и APK install, открытие UI, exact package launch и supervision/cleanup принадлежащей процессной группы.
- `tests/waydroid_cli_test.sh` — synthetic end-to-end проверка собранного CLI: status, отказ без consent, reuse внешней сессии, запуск, APK install, package launch и SIGTERM cleanup. Файл APK синтетический и никогда не передаётся реальному Android.

`make_emulator_command()` только формирует аргументы; он не проверяет и не устанавливает SDK/AVD. `start_waydroid_session()` требует уже установленный/инициализированный Waydroid, текущую Wayland/D-Bus сессию и отдельное подтверждение; он не скачивает образ и не изменяет систему напрямую. Ошибки запуска имеют машинно-читаемые коды `android.*`; аргументы передаются напрямую через `argv`, пользовательские строки не интерпретируются shell.

## Этап 2: non-destructive runtime preflight

Добавлены новые файлы:

- `include/bonk/android/preflight.hpp` — `RuntimePreflightRequest`, `PathProbe`, `ProbeState`, `RuntimePreflightReport`.
- `src/android_engine/preflight.cpp` — read-only проверка явно выбранных абсолютных путей emulator/ADB/SDK/AVD/KVM, чтение `path` из AVD `.ini`, `image.sysdir.1` из `config.ini`, проверка доступности `system.img` и защита от пути system image вне выбранного SDK root.
- `tests/android_engine/preflight_test.cpp` — классификация отсутствующих/относительных путей, AVD metadata и пути образа.

Preflight не запускает найденные executable, не загружает и не меняет файлы, не читает содержимое образа, не проверяет его лицензию, формат или bootability. `static_inputs_present()` означает только наличие перечисленных файлов. Доступность `/dev/kvm` — только проверка типа устройства и прав чтения/записи; наличие KVM-ускорения окончательно не подтверждается до реального runtime smoke test. Синтетические файлы из юнит-теста — только фикстуры путей/INI и никогда не передаются эмулятору.

## Этап 3: ADB guest-state probe

Добавлены:

- `include/bonk/android/process_runner.hpp` и `src/android_engine/process_runner.cpp` — запуск коротких host-команд через `posix_spawn` по абсолютному пути, без shell/PATH-поиска, с раздельным stdout/stderr, лимитом до 64 MiB на поток, таймаутом и завершением/сбором дочернего процесса.
- `include/bonk/android/adb_client.hpp` и `src/android_engine/adb_client.cpp` — pure argv builders, parsers для полной выдачи `adb devices -l` и выбранного serial, parser `sys.boot_completed`, `list_adb_devices()` и `query_guest_status()`.
- `tests/android_engine/process_runner_test.cpp` и `tests/android_engine/adb_client_test.cpp`.

`list_adb_devices()` не требует выбранного serial: он возвращает строки `adb devices -l` с состояниями, а повторяющийся serial маркирует как `Unknown`, чтобы вызывающий код мог запросить однозначный выбор. `query_guest_status()` отдельно проверяет переданный serial через `adb devices -l`; только для состояния `device` читает `adb -s SERIAL shell getprop sys.boot_completed`. Ответы различают `NotConnected`, `Offline`, `Unauthorized`, `AdbOnline`, `Booting`, `BootComplete` и `Unknown`. `BootComplete` значит только, что свойство Android равно `1`; это не готовность приложения или игры. Эти текстовые ADB-запросы ограничены 64 KiB вывода на поток и таймаутом не более 60 секунд на команду.

Запуск `adb` может автоматически поднять пользовательский ADB server и создать `~/.android/adbkey`. Поэтому `AdbConfig::allow_adb_server_autostart` по умолчанию `false`; каждый API, запускающий ADB, откажет до процесса, пока вызывающий код не выставит `true` после явного согласия пользователя. На этом этапе реальные ADB/AVD/Waydroid отсутствовали.

## Этап 4: одиночный PNG snapshot и ограниченные команды ввода

Добавлено в ADB bridge и PNG-декодер:

- `make_adb_screenshot_command()` строит `adb -s SERIAL exec-out screencap -p`; `capture_screenshot_rgba()` запускает одну такую команду и возвращает декодированную RGBA-картинку.
- `decode_screenshot_png()` ограничивает вход 32 MiB и изображение 16 777 216 пикселями (по стороне не более 16384 px), принимает 8-bit grayscale/RGB/grayscale-alpha/RGBA, проверяет границы всех PNG chunks, CRC, последовательность IDAT и обязательный финальный IEND. Это один снимок, не видеопоток.
- `make_adb_tap_command()` / `send_tap_normalized()` отправляют один `input tap`; `make_adb_swipe_command()` / `send_swipe_normalized()` — один прямолинейный `input swipe` с явно ограниченной длительностью 1 ms..30 s. Конечные нормализованные координаты `[0,1]` округляются к пикселям `[0,width-1] × [0,height-1]`; viewport ограничен 1..16384 px по стороне. Таймаут ADB-команды swipe обязан превышать длительность жеста.
- Android shell синтезирует swipe между начальной и конечной точкой, но приложение не управляет каждым DOWN/MOVE/UP и не подтверждает их доставку. Нет произвольной траектории, pointer ID, cancel, multitouch, joystick или удерживаемого ввода между вызовами. `make_adb_key_command()` / `send_android_key()` отправляют один numeric `input keyevent` (код 1..65535); KeyDown/KeyUp и удержание клавиши отсутствуют.
- Подключение старого Android-агента не реализовано. Установка выбранного APK описана на этапе 7; запуск установленного приложения по package ID — на этапе 5.
- Все ADB-операции, как и guest-state query, требуют `allow_adb_server_autostart=true` после явного согласия. Таймаут каждой команды — от 1 ms до 60 s; screenshot stdout/stderr ограничены 32 MiB на поток, обычные ADB-команды — 64 KiB на поток.
- `include/bonk/image/image.hpp`, `src/image/inflate.cpp`, `src/image/png.cpp`: добавлены лимитированный inflate и overload PNG decode с пределом пикселей; PNG decoder также ограничивает распакованные и RGBA-буферы 256 MiB.
- `tests/android_engine/adb_client_test.cpp` проверяет fixture PNG и fake ADB executable, точные argv/capture/tap/swipe/key/install/launch, явные install/launch failure reports и bounded boot polling; `tests/image/png_test.cpp` проверяет лимиты inflater/декодера. Fake ADB и `tests/fixtures/logo.png` — только синтетические тестовые данные, не проверка настоящего Android.

Настоящие `adb`, Emulator/AVD и Waydroid в окружении отсутствуют, поэтому нет live-проверки формата кадров и успешной доставки tap/swipe/keyevent на госте. ADB user server может быть запущен при первом вызове; запрет по умолчанию сохраняется.

## Этап 5: запуск уже установленного приложения

- `make_adb_launch_package_command()` формирует `adb -s SERIAL shell am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -p PACKAGE`.
- `launch_installed_package()` проверяет dot-separated Android package ID, требует ADB consent и ограничивает timeout 60 секундами. Используется только package ID, предоставленный/подтверждённый вызывающей стороной; имя APK не используется для догадки.
- Соседний `query_installed_package_paths()` строит `adb -s SERIAL shell pm path PACKAGE` и возвращает абсолютные guest-side пути базового и split APK. Это отдельный package-presence probe, не извлечение APK на хост и не проверка launchability.
- Функция **не устанавливает APK** и не проверяет фактическую готовность приложения. Явный `Error:` или `Status:` не `ok` из stdout/stderr `am start -W` имеет приоритет и возвращает `android.app_launch_reported_failure`, даже если ADB завершился с ненулевым кодом; при отсутствии таких строк проверяется обычный exit status. Даже `Status: ok` не доказывает готовность приложения или игры.
- Fake ADB проверяет argv, consent gating, явные launch failure строки в обоих потоках и их приоритет над ненулевым exit code, но не подтверждает наличие пакета, выбор launcher activity или успешный запуск в Android.
- `parse_adb_package_paths_output()` ограничивает текст 64 KiB, принимает только `package:/absolute/path` строки и возвращает guest пути без префикса. Явный `Error: package not found` получает `android.package_not_installed` даже при ненулевом exit code; malformed output, timeout и прочие ошибки остаются ошибками. Пустой вектор означает лишь, что успешный ответ не содержал путей.

Этот ADB API остаётся самостоятельным host-side строительным блоком для SDK/ADB backend; CLI Waydroid запускает приложения отдельным Waydroid-native adapter через `waydroid app launch`. Реальные package/APK operations и live smoke test на Android всё ещё требуют целевого хоста.

## Этап 6: bounded ожидание загрузки гостя

- `wait_for_guest_boot()` повторяет guest-state query, пока выбранный serial не сообщит `sys.boot_completed=1`; любые неготовые состояния гостя продолжают опрашиваться до общего дедлайна.
- `wait_for_runtime_and_guest_boot()` принимает уже запущенный `EngineSession`, проверяет его состояние до и после каждого ADB query и возвращает `android.runtime_exited_during_boot`, если runtime завершился до готовности гостя. Если сессия изначально не запущена, возвращается `android.runtime_not_running`, причём ADB не вызывается. Функция сама не стартует и не останавливает runtime; `EngineSession` остаётся однопоточным и принадлежит вызывающему потоку.
- По умолчанию общий timeout — 120 s, интервал — 500 ms, timeout каждой ADB-команды — 5 s. Общий timeout ограничен пятью минутами, polling interval — 1 ms..30 s; бюджет одного query уменьшается с учётом того, что он может запустить две ADB-команды. Ошибка timeout содержит последнее наблюдённое состояние.
- Ожидание не запускает Waydroid, Emulator/AVD и не устанавливает/запускает приложение. `sys.boot_completed=1` — только boot property, а не готовность лаунчера или игры; всё ещё нужен live smoke test.
- Fake ADB проверяет последовательность `0 → 1`, consent и timeout при отсутствующем serial; тесты дополнительной обёртки покрывают отказ до ADB без работающей runtime-сессии, успешное ожидание и раннее завершение процесса. Это не подтверждение настоящей загрузки Android.

## Этап 7: установка выбранного APK

- `install_selected_apk()` принимает только явно указанный абсолютный путь к существующему читаемому обычному файлу. Команда `adb -s SERIAL install PATH` передаёт путь отдельным argv без shell и без флагов replace/downgrade.
- Для каждой установки вызывающий код обязан передать `user_confirmed_install=true` только после отдельного подтверждения пользователем именно этого файла. Дополнительно требуется `allow_adb_server_autostart=true`; два согласия независимы. В текущем checkout нет UI, который собирает эти подтверждения.
- Функция не скачивает APK, не выводит package ID из имени, не проверяет содержимое/подпись заранее и не запускает установленное приложение. Явный `Failure...` в stdout/stderr получает `android.apk_install_reported_failure` независимо от exit code; если маркера нет, применяется обычная проверка процесса. Android Package Manager выполняет собственную проверку при установке, live-установка здесь не проверялась.
- Тестовый APK payload и Fake ADB проверяют только валидацию пути, argv, consent gates и распознавание явного failure в обоих потоках до обработки ненулевого exit code; синтетический payload не является устанавливаемым APK.

## Зависимости, лицензии и происхождение

### Добавлено сейчас

- Никаких сторонних библиотек и готового Android-кода.
- Используются C++20, POSIX `posix_spawn`, `waitpid`, process groups и сигналы. Все исполняемые файлы задаются абсолютным путём, shell и PATH-поиск не используются.
- Android SDK, ADB, runtime binaries, system images, Google Play Services и APK не включены в репозиторий. Никакой образ не загружался.

### Рассмотренные компоненты (код не копируется и не связывается)

- **Waydroid** — выбранный Linux runtime-кандидат. [Upstream README](https://github.com/waydroid/waydroid) описывает Android в Linux-контейнере и доступ к host hardware; upstream CLI/session код подтверждает Wayland/D-Bus модель: [session manager](https://github.com/waydroid/waydroid/blob/main/tools/actions/session_manager.py), [app manager](https://github.com/waydroid/waydroid/blob/main/tools/actions/app_manager.py). Репозиторий лицензирован GPL-3.0; Bonk пока только запускает явно выбранный внешний CLI, Waydroid код/images не копирует и не связывает. Перед bundling/distribution нужен отдельный license/dependency review.
- **Android SDK Emulator / AVD** — внешний пользовательский optional test backend. Источники: [официальная документация по аргументам emulator](https://developer.android.com/studio/run/emulator-commandline) и [условия Android Studio/SDK](https://developer.android.com/studio/terms). На этом этапе пакет не распространяется; перед любым bundling надо проверить лицензии именно выбранной версии SDK, system image, notices и права на распространение. Пользователь сам выбирает образ и подтверждает применимые условия.
- **QEMU** — проверен текст лицензии в [репозитории QEMU](https://github.com/qemu/qemu/blob/master/LICENSE): QEMU как целое выпущен под GPL version 2; файлы могут иметь дополнительные совместимые лицензии, а распространяемый пакет содержит отдельно лицензируемые компоненты/firmware. QEMU не добавлен и не связывается. Прямое включение или распространение потребует отдельного покомпонентного license review и выполнения условий GPL.
- **Cuttlefish** — проверен файл [LICENSE проекта](https://github.com/google/android-cuttlefish/blob/main/LICENSE): лицензия задаётся по перечисленным путям/файлам и не является единой лицензией на весь runtime. Cuttlefish не добавлен; перед выбором потребуется аудит всего нужного набора исходников, бинарных компонентов и образов.

Так как код внешних runtime-компонентов не включён, их copyright/NOTICE файлы в данный этап не копируются. Это не разрешение на дальнейшее распространение. Bonk не устанавливает Waydroid, не выполняет `waydroid init`, не скачивает system/vendor images, не вызывает `sudo`, не загружает Binder modules, не меняет device permissions и не меняет `/dev/kvm`.

## Происхождение образов и ограничения

Android images в репозиторий не добавляются. Пользователь самостоятельно устанавливает и инициализирует Waydroid из проверенного источника; никаких автоматических загрузок при старте Bonk нет. Для внешнего/custom image надо сохранить build/channel, происхождение и SHA-256. SDK Emulator/AVD остаётся optional test backend; при его использовании пользователь выбирает официальный SDK package и принимает его лицензию отдельно.

Ограничения, которые должны оставаться видимыми до тестирования:

- Waydroid зависит от конкретной Linux kernel/configuration: Wayland socket, D-Bus session, Binder support, GPU render node и права устройств должны работать. Preflight и live test на реальном desktop-хосте ещё не реализованы.
- ABI image важен: x86_64 Android environment не гарантирует запуск ARM-only native libraries; наличие совместимого ARM translation слоя нельзя предполагать.
- GPU/Vulkan/OpenGL, resolution, suspend/resume, audio и latency требуют проверки на конкретном host+image.
- Google Play Services, Play Integrity, DRM, anti-cheat и политика игры могут блокировать Android container/virtual device. Никаких game APK или Google components в проекте нет.
- Нет измерений FPS и совместимости со Standoff 2, Free Fire или другими играми. «Любые игры» и преимущества BlueStacks не заявляются без тестов.

## Безопасное сопоставление раскладок

В текущем checkout нет модели раскладок, APK-редактора или APK-привязок, поэтому мигрировать нечего и чужие данные не менялись. Для интеграции с агентом лаунчера предлагается только добавочная схема:

1. Добавить необязательное поле Android `package_id` (`applicationId`) к новой записи раскладки; существующий `apk_path`/legacy key сохранить.
2. Новые раскладки сохранять по точному package ID (при необходимости — вместе с подписью/сертификатом приложения), а не по имени файла.
3. Старую раскладку переносить только после чтения package ID из AndroidManifest APK или установленного пакета и явного подтверждения совпадения. Не выводить идентификатор из имени APK.
4. Сначала создать копию/новую запись с source/backup metadata; старую не удалять. При нескольких APK с тем же package ID, несовпадающих сертификатах или коллизии — показать выбор пользователю, не перезаписывать автоматически.
5. Оставить fallback чтения старых APK-keyed записей на переходный срок и добавить тесты миграции до изменения схемы редактора.

Эта миграция требует согласования с агентом редактора и в код первого этапа не входит.

## Наблюдаемое состояние движка (layered snapshot)

`bonk waydroid` больше не печатает одну строку session/container. Каждый слой наблюдается отдельно и печатается стабильными machine-readable строками (`src/android_engine/engine_state.cpp`):

```text
engine runtime=stopped container=unknown owned=no
engine guest=unknown observed=no serial=-
engine app=not-requested package=- foreground-confirmed=no
engine input sent=no observed-in-guest=no
engine frames=unavailable
```

Инварианты, закреплённые тестами `engine_state` и синтетическим CLI-тестом:

- `runtime=running` ничего не говорит о госте: `guest` остаётся `unknown observed=no`, пока ADB-запрос реально не выполнился. Неопрошенный гость никогда не рендерится как `boot-complete`, даже если вызывающий код выставил такое значение в структуре.
- `guest=boot-complete` означает только `sys.boot_completed=1`, а не готовность приложения.
- `app=launch-command-succeeded` — результат команды. `foreground-confirmed=yes` требует свидетеля внутри Android и никогда не выводится из нулевого exit code.
- `input sent=yes` относится к хостовой команде; `observed-in-guest=yes` требует подтверждения внутри гостя.
- `frames=unavailable` не выводится из живого гостя: одиночный PNG не является потоком.

Разбор строк для evidence: `engine detail ...` содержит свободный текст и не участвует в сравнении state-токенов.

### Опциональный guest probe (ADB)

Read-only наблюдение гостя включается явно и только с отдельным согласием, потому что обращение к ADB может поднять user-level ADB server:

```sh
./build/bonk waydroid --adb /absolute/path/adb --confirm-adb \
  --adb-serial 192.168.240.112:5555
```

- `--adb` без `--confirm-adb` отклоняется на разборе аргументов (`cli.usage`); ADB-процесс не запускается.
- `--adb-serial` выбирает transport для probe и сам по себе не включает ADB.
- Провал probe не выдумывает состояние: `guest` остаётся `unknown observed=no`, причина печатается в `engine detail guest=guest probe failed: ...`, остальной вывод команды выполняется дальше.
- Probe не стартует сессию, не устанавливает APK и не запускает приложение.

## Контракт порядка действий (launcher plan)

Порядок шагов и правила согласия вынесены из `main.cpp` в чистую функцию `plan_launcher()` (`include/bonk/android/launcher_plan.hpp`), поэтому контракт проверяется юнит-тестами без Waydroid-хоста. Список шагов печатается как данные, а CLI исполняет именно его:

```text
start-session -> wait-for-session -> [probe-guest] -> open-ui
              -> install-apk -> launch-package -> await-owned-session
```

Правила, закреплённые тестами `launcher_plan` и синтетическим CLI-тестом:

- Валидация (`android.invalid_package_name`, `android.apk_install_consent_required`) выполняется **до** любого обращения к runtime: отклонённый запрос не может стартовать сессию как побочный эффект.
- Действия при остановленной сессии без `--confirm-start` отклоняются (`android.waydroid_start_consent_required`); никакой шаг не выполняется.
- Если сессия уже запущена, `--confirm-start` не даёт владения: план помечается `declined_ownership`, шагов `start-session` и `await-owned-session` нет.
- `await-owned-session` появляется только когда сессию стартовал сам Bonk. Поэтому Ctrl-C останавливает только собственную process group.
- `probe-guest` — read-only и не требует running-сессии: probe-only запрос (только `--adb ... --confirm-adb`) не открывает UI и не стартует runtime, даже если сессия остановлена.
- `open-ui` предшествует установке и запуску: приложение запускается только после появления поверхности отображения.
- `install-apk` и `launch-package` несут собственные `detail`-значения, поэтому CLI не может подменить APK или package ID между шагами.

## Подтверждение foreground внутри гостя

`foreground-confirmed=yes` — единственный токен, который означает «Android сообщает, что запущенный пакет сейчас на переднем плане». Он выставляется только запросом к гостю, а не exit code команды.

```sh
./build/bonk waydroid --launch-package com.example.game \
  --adb /absolute/path/adb --confirm-adb --adb-serial "$SERIAL"
```

- Команда гостя: `adb -s SERIAL shell dumpsys activity activities` (`make_adb_foreground_activity_command`).
- Парсер `parse_activity_dump_shows_foreground_package()` принимает маркеры `mResumedActivity`, `topResumedActivity` и `ResumedActivity` (форматы различаются между версиями Android) и требует `<package>/<activity>` с целыми границами.
- Ищется только часть строки **после** маркера, поэтому имя пакета, упомянутое раньше в строке, не может «одолжить» маркер.
- Префикс пакета не совпадает: `id.lynxdroid` не подтверждает `id.lynxdroid.agent`; фоновые записи `Hist #` и `Task{...}` не считаются foreground.
- Похожие поля (`mLastResumedActivity`) не принимаются за маркер.
- `dumpsys` — диагностический дамп и может завершиться с ненулевым кодом, поэтому решение принимает разбор: нераспознанный дамп даёт `foreground-confirmed=no`, а не успех.
- Провал запроса не выставляет подтверждение: причина попадает в `engine detail guest=foreground check failed: ...`.

Синтетический CLI-тест проверяет оба исхода (resumed — игра, resumed — лаунчер) и мутационная проверка подтвердила, что тест падает, если парсер начинает подтверждать без свидетеля. Это всё ещё **не** live-проверка: fake ADB не является Android-гостем.

## Input provider: валидированные действия

Ввод моделируется как типизированное действие (`include/bonk/android/input_action.hpp`), а не как набор координат и пауз:

| Действие | Требует | Отклоняется |
|---|---|---|
| `make_tap(viewport, x, y)` | захваченный viewport 1..16384 px; `x,y` конечные в `[0,1]` | нулевой/полузаданный/огромный viewport (`android.invalid_viewport`), NaN/∞ или выход за `[0,1]` (`android.invalid_coordinates`) |
| `make_swipe(viewport, start, end, duration)` | то же плюс `duration` 1 ms..30 s | те же коды плюс `android.invalid_gesture_duration` |
| `make_key(code)` | `code` 1..65535 | `android.invalid_key_code` |

- Действие несёт **viewport, против которого оно было снято**. При смене разрешения/поворота нужно построить новое действие; старая нормализованная точка не «переедет» молча в другое место.
- `build_input_command()` сам валидирует действие, поэтому обойти проверку, собрав команду напрямую, нельзя.
- `send_input_action()` возвращает структуру с `command_sent` и `observed_in_guest`. После успешного `adb shell input` `command_sent=yes`, а `observed_in_guest` **остаётся `false`**: транспорт принял запрос, но никто внутри Android его не подтвердил.
- Отправка требует ADB-consent, как и остальные ADB-вызовы; без него процесс не запускается.
- `sequence` передаётся вызывающим кодом, чтобы позже сопоставить принятое действие со свидетельством из гостя.

Таким образом слой `input` в engine snapshot — про транспорт, а не про доставку в игру. Gate «событие ввода внутри Android» остаётся открытым до появления test-app/agent, который запишет полученный tap/swipe/key.

## Runbook: первый live smoke test на целевом Linux-хосте

Этот сценарий **не запускался в sandbox**. На тестовом ПК Waydroid должен быть установлен и инициализирован отдельно; выполнять команды от desktop-пользователя внутри его Wayland/D-Bus-сессии. Сначала проверять на выбранном безопасном приложении, затем на конкретной лицензированной игре.

```sh
make -j2 all
waydroid status                         # baseline, не меняет состояние
./build/bonk waydroid                   # Bonk status-only query
./build/bonk waydroid --confirm-start   # явный старт, UI и foreground supervision
```

Для уже установленного приложения запусти отдельный процесс команды с точным ID пакета:

```sh
./build/bonk waydroid --confirm-start --launch-package com.example.game
```

Для выбранного APK требуются абсолютный путь и отдельное подтверждение именно этого файла:

```sh
./build/bonk waydroid --confirm-start \
  --install-apk /absolute/path/game.apk --confirm-install \
  --launch-package com.example.game
```

После старта сессии сними guest-слой отдельным read-only probe и сохрани вывод как evidence:

```sh
./build/bonk waydroid --adb /absolute/path/adb --confirm-adb \
  --adb-serial "$SERIAL" | tee guest-probe.log
grep '^engine guest=' guest-probe.log      # ожидается boot-complete observed=yes
```

До `boot-complete observed=yes` установка и запуск приложения не считаются проверенными, даже если команды завершились успешно.

Bonk ждёт только пока `waydroid status` покажет running (до 120 секунд), а не пока Android или игра готовы. При старте Bonk остаётся foreground; Ctrl-C посылает stop только принадлежащей ему process group, что может затронуть общий Waydroid container. Сверь `waydroid status` до и после выхода. Зафиксируй distro/kernel, Waydroid/image version и ABI, GPU/driver, APK/game version и package ID, CLI output, видимый результат запуска и факт остановки. Не считать успешный exit `app launch` доказательством готовности игры.

## Следующие проверяемые этапы

1. **Waydroid live game smoke test:** выполнить runbook выше на реальном Wayland desktop с Binder/GPU и тестовым APK. Нужны настоящий Wayland host, Waydroid image, GPU и APK; текущая sandbox среда их не имеет.
2. **Game-control mapping:** только после runtime smoke test определить модель клавиатура/мышь → Android touch. DOWN/MOVE/UP, удержания и multitouch требуют подтверждённого input transport; старый агент остаётся заблокирован до согласования namespace/address, проверки клиента и версии протокола.
3. **Видеоповерхность:** сначала использовать Waydroid own window; если требуется BlueStacks-style embedded surface, выбрать/реализовать Wayland compositor/frame IPC и измерить latency. Одиночный `adb screencap` для игр не годится.
4. **Linux preflight и владение runtime:** добавить проверки Wayland socket, Waydroid initialization, Binder/GPU и безопасной политики stop/shared-container на целевом distro; не запускать privileged setup автоматически.
5. **IPC лаунчера:** согласовать версионированный Unix socket, reconnect, раздельные process/guest states, лимиты и тесты некорректных кадров/команд.
6. **Совместимость/производительность:** тестировать на конкретном лицензированном образе/железе и каждой игре. FPS, Standoff 2, Free Fire, Google Play, anti-cheat и ARM-native compatibility не обещаются без тестов.

## Проверка

```sh
make test TEST_ARGS='--filter=android_'
make waydroid-cli-test  # synthetic CLI flow; does not require Waydroid
make verify
```

Текущая host-side проверка после добавления input provider: `make -j2 verify` — **377 тестов, 0 ошибок**, selftest PASS, synthetic CLI end-to-end PASS, `verify: OK`; ASan/UBSan — **377/377**; полный `-Werror verify` (приложение и тесты) — **377/377**. Fake Waydroid/ADB, synthetic APK и PNG fixtures не являются live Android или game compatibility tests.

Исправленная гонка в bounded boot wait: при исчерпанном общем бюджете ожидание больше не запускает ADB-процесс с ~1 ms и не сообщает `android.adb_timeout` вместо `android.guest_boot_timeout`. Если remaining budget меньше 20 ms, ожидание сразу возвращает session/boot timeout; если таймаут вызван общим deadline (`query_command_timeout < command_timeout`), он также классифицируется как expiration ожидания. Тест `android_adb.exhausted_budget_reports_boot_timeout_not_a_transport_error` и повторные прогоны под CPU-нагрузкой это закрепляют.

Для реального Waydroid smoke test нужны пользовательская Waydroid установка, Wayland/D-Bus desktop session, Binder и GPU render device, а также выбранный тестовый APK. В sandbox отсутствуют `waydroid`, `adb`, `WAYLAND_DISPLAY`, D-Bus session address, Binder device, GPU render node и `/dev/kvm`; поэтому запуск и игра на настоящем Waydroid host здесь не проверены.
