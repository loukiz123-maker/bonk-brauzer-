# Android-движок: архитектура и проверяемые этапы

**Статус:** runtime backend **не выбран**. Waydroid-адаптер и CLI-команда `bonk waydroid` удалены по решению владельца проекта от 2026-11-10: LynxDroid не должен зависеть от Waydroid. В коде остаются runtime-нейтральные host-side блоки: супервизор процесса (`EngineSession`), построитель команды для уже установленного Android SDK Emulator/AVD (необязательный тестовый backend), read-only preflight, ADB-команды (перечисление транспортов, guest-state probe с bounded wait, одиночный PNG screenshot, tap/key/swipe, пути и запуск пакета, установка выбранного APK, проверка foreground), модель наблюдаемого состояния движка, план порядка действий лаунчера и типизированные действия ввода. Всё проверено unit-тестами и тестовым ADB shim; настоящий Android-гость, ADB и AVD в этой среде не запускались. CLI-лаунчера, видеоканала и управления произвольным состоянием pointer/key нет. Архитектурный draft: [docs/LYNXDROID_ARCHITECTURE.md](LYNXDROID_ARCHITECTURE.md).

## Исходное состояние репозитория

Текущий checkout — C++20-проект **Bonk Brauzer**, а не отдельный репозиторий LynxDroid. В нём есть HTML/CSS/JS-движок и X11-оболочка, но нет Android runtime, Waydroid, scrcpy, лаунчера, APK-раскладок или кода редактора управления. Поэтому этот этап добавляет отдельный host-side модуль и не меняет браузерную оболочку или чужие файлы интерфейса.

Сборка проекта подхватывает новые `src/*/*.cpp` и `tests/*/*.cpp` автоматически. Проверенный до изменений базовый результат: `make -j2 verify` — 271 тест, ошибок нет.

## Архитектурное решение

### Runtime backend: не выбран

Игровой runtime для LynxDroid сейчас **не выбран**, и ни один не подключён к коду как рабочий backend.

- **Waydroid — исключён.** Некоторое время он был кандидатом, и в этой ветке существовали адаптер `waydroid_runtime` и команда `bonk waydroid`. Они удалены: требование проекта — Android-движок для Linux без Waydroid. Не возвращать без нового явного решения владельца; тест `cli.has_no_waydroid_command_or_options` это закрепляет.
- **Android SDK Emulator с уже созданным пользователем AVD** — остаётся только как необязательный тестовый backend. Код лишь строит argv `emulator -avd <name> [-no-window]` и управляет жизненным циклом foreground-процесса. Он не объявлен игровым runtime: это developer-oriented VM, производительность и совместимость игр не измерялись.
- **Другие варианты** (собственная KVM-VM, Cuttlefish/crosvm, QEMU) не реализованы и не выбраны; сведения о лицензиях — ниже.

Выбор runtime — открытое архитектурное решение. До него нельзя писать в документации или интерфейсе, что движок запускает Android.

Любой runtime и образ должны быть установлены/созданы пользователем самостоятельно. Приложение не скачивает образы, не принимает лицензии за пользователя, не устанавливает системные пакеты, не вызывает `sudo` и не меняет системные настройки.

### Границы модулей

```text
Лаунчер
  ├─ запросы start / stop / status
  ├─ центральная поверхность: кадры Android
  └─ touch / key input
       │
       ▼
Android bridge (host-side API; IPC лаунчера ещё нет)
  ├─ EngineSession: владеет только созданным runtime-процессом
  ├─ Guest monitor: отдельно определяет booting / ready / failure
  ├─ Frame source: один PNG screenshot → RGBA (не видеопоток)
  └─ Input sink: tap/key и единичный прямолинейный swipe через ADB (не произвольный путь или joystick)
       │
       ▼
Runtime backend (не выбран; сейчас есть только command builder для SDK Emulator + AVD)
```

`EngineSession::start()` означает только, что ОС создала процесс. `ProcessState::Running` **не** означает, что Android загрузился, что лаунчер получил кадр или что игра запущена. `list_adb_devices()`, `query_guest_status()`, `wait_for_guest_boot()`, `capture_screenshot_rgba()`, `send_tap_normalized()`, `send_swipe_normalized()`, `send_android_key()`, `query_installed_package_paths()` и `launch_installed_package()` выполняют отдельные ADB-команды; все требуют явного согласия перед возможным запуском пользовательского ADB server. Настоящий AVD в этой среде отсутствует.

## Контракт лаунчера

Ниже перечислены текущие реализации и целевые требования; статус каждой строки указан явно.

| Направление | Контракт |
|---|---|
| Запуск | `start(profile_id)`; профиль указывает на уже существующий AVD. Ответ содержит отдельные `runtime_state` и `guest_state`. |
| Остановка | `stop(grace_period)`; сначала штатное завершение, затем явная эскалация. Остановка затрагивает только процессную группу, созданную движком. |
| Состояние | `Stopped`, `Starting`, `RuntimeRunning`, `GuestBooting`, `Ready`, `Stopping`, `Failed`; поля runtime и guest нельзя смешивать. В текущем коде доступны состояния процесса `stopped/running/exited/failed`; `wait_for_guest_boot()` опрашивает ADB до `sys.boot_completed=1`, а `wait_for_runtime_and_guest_boot()` дополнительно обнаруживает завершение принадлежащего `EngineSession` runtime во время ожидания. Ни один вызов не означает готовность игры и не заменяет непрерывный монитор состояния. |
| Кадры | Сейчас `capture_screenshot_rgba()` возвращает один декодированный PNG RGBA без sequence/timestamp/rotation; лимит 32 MiB сжатого PNG и 16 777 216 пикселей. Это не видеопоток и не гарантия низкой задержки. Будущий IPC/frame API должен добавить stride, sequence, monotonic timestamp и rotation. |
| Touch | Есть одиночный tap и прямолинейный swipe через `send_swipe_normalized()`: обе пары координат нормализованы в `[0,1]` и округляются в пиксели `[0,width-1] × [0,height-1]`; swipe длится 1 ms..30 s. ADB shell просит Android синтезировать жест, но доставка Down/Move/Up в приложение не подтверждена live-тестом. Нет caller-controlled событий, произвольной траектории, pointer ID, отмены, multitouch или joystick; letterbox/rotation учитывает вызывающий UI. |
| Клавиши | Сейчас только одиночный `adb shell input keyevent <numeric-code>` (1..65535); KeyDown/KeyUp, repeat и удержание не реализованы. Не передавать сырые клавиши хоста как Android-коды без явного преобразования. |
| Запуск приложения | `launch_installed_package()` запрашивает MAIN/LAUNCHER activity уже установленного приложения по подтверждённому package ID; эта функция APK не устанавливает. Успешный exit `am start` не доказывает готовность приложения/игры. |

Когда лаунчер и движок будут разнесены по процессам, предпочтителен версионированный **Unix-domain socket** в приватном runtime-каталоге: управляющие сообщения отдельно от кадров, кадры — бинарные с длиной и метаданными, без base64 в JSON. Не открывать неаутентифицированный TCP-порт. Если UI работает в браузерном preview, браузерный код не должен обращаться к `localhost`; нужен native IPC или same-origin proxy, принадлежащий приложению.

### Блокер интеграции со старым Android-агентом

По предоставленному QA-отчёту, старый `TouchAgentService` создаёт `LocalServerSocket(String)` в абстрактном namespace Android, тогда как Python-клиент подключается к файловому pathname. Эти адресации несовместимы; C++-модуль пока не содержит IPC-клиента и не должен подключаться к агенту, пока стороны не согласуют namespace/address, проверку клиента и версию протокола. Нужен явный handshake и проверка peer/прав до передачи команд. **Решение на текущий этап: интеграцию отложить**; C++ host-side API остаётся самостоятельным до появления исходников агента, согласованного IPC-протокола, выбранного runtime на целевом хосте и тестового APK для совместного smoke test. Источники из отчёта: [TouchAgentService.java](https://github.com/loukiz123-maker/bonk-brauzer-/blob/8d13e22/android_agent/app/src/main/java/id/lynxdroid/agent/TouchAgentService.java#L88-L118) и [Android LocalServerSocket API](https://developer.android.com/reference/android/net/LocalServerSocket).

Реализованы generic EngineSession, optional SDK Emulator/AVD command builder, ADB guest-state query и bounded boot wait, установка выбранного APK, запуск установленного приложения и проверка foreground, одиночный PNG snapshot, tap/key/swipe, модель состояния движка, план действий лаунчера и типизированные действия ввода. Runtime-адаптера и CLI-лаунчера нет (Waydroid-адаптер и `bonk waydroid` удалены). IPC-сокет, непрерывный guest monitor, поток кадров, low-latency video и game-control mapping остаются незавершёнными; настоящий Android и игры не тестировались.

## Этап 1: выполненная работа

Новые файлы:

- `include/bonk/android/engine_session.hpp` — типы `RuntimeCommand`, `EmulatorConfig`, `ProcessState`, `SessionStatus` и класс `EngineSession`.
- `src/android_engine/engine_session.cpp` — `posix_spawn` с явным абсолютным путём (без shell/PATH-поиска), отдельная process group, non-blocking `poll`, `SIGTERM` → `SIGKILL`, сбор exit code/signal и очистка дочернего процесса в деструкторе.
- `tests/android_engine/engine_session_test.cpp` — тесты командной строки, ошибок отсутствующего executable, повторного запуска, штатного выхода, NUL в аргументах и остановки тестового `/bin/sleep`.

`make_emulator_command()` только формирует аргументы; он не проверяет и не устанавливает SDK/AVD. Ошибки запуска имеют машинно-читаемые коды `android.*` и текст системной ошибки. Командные аргументы передаются напрямую через `argv`; пользовательские строки не интерпретируются shell.

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

Запуск `adb` может автоматически поднять пользовательский ADB server и создать `~/.android/adbkey`. Поэтому `AdbConfig::allow_adb_server_autostart` по умолчанию `false`; каждый API, запускающий ADB, откажет до процесса, пока вызывающий код не выставит `true` после явного согласия пользователя. На этом этапе реальные ADB/AVD отсутствовали.

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

Настоящие `adb`, Emulator и AVD в окружении отсутствуют, поэтому нет live-проверки формата кадров и успешной доставки tap/swipe/keyevent на госте. ADB user server может быть запущен при первом вызове; запрет по умолчанию сохраняется.

## Этап 5: запуск уже установленного приложения

- `make_adb_launch_package_command()` формирует `adb -s SERIAL shell am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -p PACKAGE`.
- `launch_installed_package()` проверяет dot-separated Android package ID, требует ADB consent и ограничивает timeout 60 секундами. Используется только package ID, предоставленный/подтверждённый вызывающей стороной; имя APK не используется для догадки.
- Соседний `query_installed_package_paths()` строит `adb -s SERIAL shell pm path PACKAGE` и возвращает абсолютные guest-side пути базового и split APK. Это отдельный package-presence probe, не извлечение APK на хост и не проверка launchability.
- Функция **не устанавливает APK** и не проверяет фактическую готовность приложения. Явный `Error:` или `Status:` не `ok` из stdout/stderr `am start -W` имеет приоритет и возвращает `android.app_launch_reported_failure`, даже если ADB завершился с ненулевым кодом; при отсутствии таких строк проверяется обычный exit status. Даже `Status: ok` не доказывает готовность приложения или игры.
- Fake ADB проверяет argv, consent gating, явные launch failure строки в обоих потоках и их приоритет над ненулевым exit code, но не подтверждает наличие пакета, выбор launcher activity или успешный запуск в Android.
- `parse_adb_package_paths_output()` ограничивает текст 64 KiB, принимает только `package:/absolute/path` строки и возвращает guest пути без префикса. Явный `Error: package not found` получает `android.package_not_installed` даже при ненулевом exit code; malformed output, timeout и прочие ошибки остаются ошибками. Пустой вектор означает лишь, что успешный ответ не содержал путей.

Этот API — host-side строительный блок; запуск APK из библиотеки лаунчера и live smoke test остаются отдельными задачами.

## Этап 6: bounded ожидание загрузки гостя

- `wait_for_guest_boot()` повторяет guest-state query, пока выбранный serial не сообщит `sys.boot_completed=1`; любые неготовые состояния гостя продолжают опрашиваться до общего дедлайна.
- `wait_for_runtime_and_guest_boot()` принимает уже запущенный `EngineSession`, проверяет его состояние до и после каждого ADB query и возвращает `android.runtime_exited_during_boot`, если runtime завершился до готовности гостя. Если сессия изначально не запущена, возвращается `android.runtime_not_running`, причём ADB не вызывается. Функция сама не стартует и не останавливает runtime; `EngineSession` остаётся однопоточным и принадлежит вызывающему потоку.
- По умолчанию общий timeout — 120 s, интервал — 500 ms, timeout каждой ADB-команды — 5 s. Общий timeout ограничен пятью минутами, polling interval — 1 ms..30 s; бюджет одного query уменьшается с учётом того, что он может запустить две ADB-команды. Ошибка timeout содержит последнее наблюдённое состояние.
- Ожидание не запускает Emulator/AVD и не устанавливает/запускает приложение. `sys.boot_completed=1` — только boot property, а не готовность лаунчера или игры; всё ещё нужен live smoke test.
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

- **Waydroid** — рассматривался и **исключён** решением владельца проекта (LynxDroid не должен от него зависеть). Проект лицензирован GPL-3.0; его код и образы никогда не копировались и не связывались, адаптер к его CLI удалён.
- **Android SDK Emulator / AVD** — внешний пользовательский пакет. Источники: [официальная документация по аргументам emulator](https://developer.android.com/studio/run/emulator-commandline) и [условия Android Studio/SDK](https://developer.android.com/studio/terms). На этом этапе пакет не распространяется; перед любым bundling надо проверить лицензии именно выбранной версии SDK, system image, notices и права на распространение. Пользователь сам выбирает образ и подтверждает применимые условия.
- **QEMU** — проверен текст лицензии в [репозитории QEMU](https://github.com/qemu/qemu/blob/master/LICENSE): QEMU как целое выпущен под GPL version 2; файлы могут иметь дополнительные совместимые лицензии, а распространяемый пакет содержит отдельно лицензируемые компоненты/firmware. QEMU не добавлен и не связывается. Прямое включение или распространение потребует отдельного покомпонентного license review и выполнения условий GPL.
- **Cuttlefish** — проверен файл [LICENSE проекта](https://github.com/google/android-cuttlefish/blob/main/LICENSE): лицензия задаётся по перечисленным путям/файлам и не является единой лицензией на весь runtime. Cuttlefish не добавлен; перед выбором потребуется аудит всего нужного набора исходников, бинарных компонентов и образов.

Так как код этих компонентов не включён, их copyright/NOTICE файлы в данный этап не копируются. Это не разрешение на их дальнейшее распространение. Установка системного Android-пакета, драйверов, правил устройств или изменение прав `/dev/kvm` автоматически не выполняются.

## Происхождение образов и ограничения

Образ Android в репозиторий не добавляется. На следующем этапе разрешённый источник по умолчанию — только выбранный пользователем пакет из официального Android SDK Repository; никаких автоматических загрузок при старте. Для каждой AVD-конфигурации нужно сохранять минимум: идентификатор SDK-пакета и версию (например, API level и ABI), путь к system image, Android build fingerprint после загрузки, SDK/образ license/NOTICE, а также SHA-256 полученного пакета или выбранного образа. Если пользователь явно выберет внешний образ, до запуска интерфейс должен запросить происхождение (проект/build ID или URL), показать лицензию и сохранить хэш; неизвестные образы не получать из сети.

Ограничения, которые должны оставаться видимыми до тестирования:

- Наличие `/dev/kvm`, права пользователя, CPU-виртуализация и возможности host GPU зависят от Linux-машины. Preflight проверяет только видимость устройства и права чтения/записи; ioctl, реальное ускорение и права устройств не меняет.
- ABI образа важен: x86_64 AVD не гарантирует запуск ARM-only native libraries; наличие ARM translation нельзя предполагать.
- GPU/Vulkan/OpenGL, разрешение, suspend/resume, аудио и latency требуют отдельной проверки на конкретном host+image.
- Google Play, Play Integrity, DRM, anti-cheat и правила отдельных приложений могут запрещать или ломать запуск в VM. Никаких игровых APK или Google components в проекте нет.
- Нет измерений FPS и совместимости с Standoff 2, Free Fire или другими играми; эти свойства не заявлены.

## Безопасное сопоставление раскладок

В текущем checkout нет модели раскладок, APK-редактора или APK-привязок, поэтому мигрировать нечего и чужие данные не менялись. Для интеграции с агентом лаунчера предлагается только добавочная схема:

1. Добавить необязательное поле Android `package_id` (`applicationId`) к новой записи раскладки; существующий `apk_path`/legacy key сохранить.
2. Новые раскладки сохранять по точному package ID (при необходимости — вместе с подписью/сертификатом приложения), а не по имени файла.
3. Старую раскладку переносить только после чтения package ID из AndroidManifest APK или установленного пакета и явного подтверждения совпадения. Не выводить идентификатор из имени APK.
4. Сначала создать копию/новую запись с source/backup metadata; старую не удалять. При нескольких APK с тем же package ID, несовпадающих сертификатах или коллизии — показать выбор пользователю, не перезаписывать автоматически.
5. Оставить fallback чтения старых APK-keyed записей на переходный срок и добавить тесты миграции до изменения схемы редактора.

Эта миграция требует согласования с агентом редактора и в код первого этапа не входит.

## Наблюдаемое состояние движка (layered snapshot)

Каждый слой наблюдается отдельно и рендерится стабильными machine-readable строками (`render_engine_snapshot()`, `src/android_engine/engine_state.cpp`). Runtime описывается нейтральными типами `RuntimeState` / `RuntimeStatus` (`include/bonk/android/runtime_status.hpp`): `session` — экземпляр runtime, которым управляет лаунчер; `container` — общий host-сервис, контейнер или VM за ним (остаётся `unknown`, если у backend такого слоя нет).

```text
engine runtime=stopped container=unknown owned=no
engine guest=unknown observed=no serial=-
engine app=not-requested package=- foreground-confirmed=no
engine input sent=no observed-in-guest=no
engine frames=unavailable
```

Это библиотечная модель с unit-тестами. Сейчас её никто не печатает: CLI-команды, которая её выводила, больше нет, а runtime backend, который бы её заполнял, не выбран.

Инварианты, закреплённые тестами `engine_state`:

- `runtime=running` ничего не говорит о госте: `guest` остаётся `unknown observed=no`, пока ADB-запрос реально не выполнился. Неопрошенный гость никогда не рендерится как `boot-complete`, даже если вызывающий код выставил такое значение в структуре.
- `guest=boot-complete` означает только `sys.boot_completed=1`, а не готовность приложения.
- `app=launch-command-succeeded` — результат команды. `foreground-confirmed=yes` требует свидетеля внутри Android и никогда не выводится из нулевого exit code.
- `input sent=yes` относится к хостовой команде; `observed-in-guest=yes` требует подтверждения внутри гостя.
- `frames=unavailable` не выводится из живого гостя: одиночный PNG не является потоком.

Разбор строк для evidence: `engine detail ...` содержит свободный текст и не участвует в сравнении state-токенов.

### Guest probe (ADB)

`make_engine_snapshot(status, owned, guest)` принимает результат `query_guest_status()`. Обращение к ADB может поднять user-level ADB server, поэтому оно требует `AdbConfig::allow_adb_server_autostart`; без согласия процесс не запускается. Провал probe не выдумывает состояние: гость остаётся `unknown observed=no`. Probe не стартует runtime, не устанавливает APK и не запускает приложение.

## Контракт порядка действий (launcher plan)

Порядок шагов и правила согласия описаны чистой функцией `plan_launcher()` (`include/bonk/android/launcher_plan.hpp`), поэтому контракт проверяется юнит-тестами без какого-либо runtime. Функция принимает `RuntimeStatus` и запрос пользователя и возвращает список шагов как данные:

```text
start-session -> wait-for-session -> [probe-guest] -> open-ui
              -> install-apk -> launch-package -> await-owned-session
```

Исполнителя у плана сейчас нет: шаги `start-session`, `open-ui`, `install-apk`, `launch-package` должен выполнять адаптер выбранного runtime, а его нет.

Правила, закреплённые тестами `launcher_plan`:

- Валидация (`android.invalid_package_name`, `android.apk_install_consent_required`) выполняется **до** любого обращения к runtime: отклонённый запрос не может стартовать сессию как побочный эффект.
- Действия при остановленной сессии без согласия на старт отклоняются (`android.runtime_start_consent_required`); никакой шаг не создаётся. При неизвестном состоянии runtime — `android.runtime_state_unknown`.
- Если сессия уже запущена, согласие на старт не даёт владения: план помечается `declined_ownership`, шагов `start-session` и `await-owned-session` нет.
- `await-owned-session` появляется только когда сессию стартует сам лаунчер. Поэтому останавливать можно только собственную process group.
- `probe-guest` — read-only и не требует running-сессии: probe-only запрос не открывает UI и не стартует runtime, даже если сессия остановлена.
- `open-ui` предшествует установке и запуску: приложение запускается только после появления поверхности отображения.
- `install-apk` и `launch-package` несут собственные `detail`-значения, поэтому исполнитель не может подменить APK или package ID между шагами.

## Подтверждение foreground внутри гостя

`foreground_confirmed` — единственный признак, который означает «Android сообщает, что запущенный пакет сейчас на переднем плане». Он выставляется только запросом к гостю, а не exit code команды.

- Команда гостя: `adb -s SERIAL shell dumpsys activity activities` (`make_adb_foreground_activity_command`).
- Парсер `parse_activity_dump_shows_foreground_package()` принимает маркеры `mResumedActivity`, `topResumedActivity` и `ResumedActivity` (форматы различаются между версиями Android) и требует `<package>/<activity>` с целыми границами.
- Ищется только часть строки **после** маркера, поэтому имя пакета, упомянутое раньше в строке, не может «одолжить» маркер.
- Префикс пакета не совпадает: `id.lynxdroid` не подтверждает `id.lynxdroid.agent`; фоновые записи `Hist #` и `Task{...}` не считаются foreground.
- Похожие поля (`mLastResumedActivity`) не принимаются за маркер.
- `dumpsys` — диагностический дамп и может завершиться с ненулевым кодом, поэтому решение принимает разбор: нераспознанный дамп не подтверждает foreground.
- Провал запроса не выставляет подтверждение.

Проверено только unit-тестами на тексте дампов и fake ADB. Это **не** live-проверка: fake ADB не является Android-гостем.

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

## Live smoke test: не выполнялся, runbook отсутствует

Прежний runbook описывал запуск через Waydroid и удалён вместе с адаптером. Новый runbook можно написать только после выбора runtime backend. До тех пор ни загрузка Android-гостя, ни запуск тестового APK, ни доставка события ввода внутрь Android не проверены ни в какой среде.

## Следующие проверяемые этапы

1. **Выбор runtime backend (без Waydroid):** решение владельца проекта и агента движка. Сравнить варианты на целевом Linux-хосте по реальным измерениям, а не по описаниям.
2. **Адаптер выбранного runtime:** статус, подтверждённый старт, остановка только своей process group, установка выбранного APK, запуск точного package ID — как исполнитель `plan_launcher()` и источник `RuntimeStatus`.
3. **Live runtime + APK smoke test:** загрузка гостя, установка явно выбранного тестового APK, запуск, snapshot и фактическая доставка событий в Android на реальном хосте.
4. **Агент ввода и протокол:** до IPC-клиента согласовать abstract-vs-filesystem socket namespace, проверку клиента и версию handshake с владельцем старого агента. Затем отдельно реализовать и проверить DOWN/MOVE/UP внутри гостя.
5. **Поток кадров:** измерить latency и выбрать кадровый транспорт для игр. Одиночный `adb screencap` не является игровым видеопотоком.
6. **IPC лаунчера:** версионированный Unix socket, reconnect, раздельные состояния процесса и гостя, лимиты и тесты некорректных кадров/команд.
7. **Совместимость/производительность:** только измерения на реальных конфигурациях, лицензированных образах и конкретных приложениях. FPS, Standoff 2, Free Fire, Google Play, anti-cheat и ARM-native compatibility не обещаются без тестов.

## Проверка

```sh
make test TEST_ARGS='--filter=android_'
make verify
```

Текущая host-side проверка после удаления Waydroid: `make -j2 verify` — **364 тестов, 0 ошибок**, selftest PASS, `verify: OK`; ASan/UBSan — **364/364**; сборка с `-Werror` — без предупреждений. Fake ADB и PNG fixtures — синтетические тесты, не live Android и не проверка совместимости игр.

Исправленная гонка в bounded boot wait: при исчерпанном общем бюджете ожидание больше не запускает ADB-процесс с ~1 ms и не сообщает `android.adb_timeout` вместо `android.guest_boot_timeout`. Если remaining budget меньше 20 ms, ожидание сразу возвращает session/boot timeout; если таймаут вызван общим deadline (`query_command_timeout < command_timeout`), он также классифицируется как expiration ожидания. Тест `android_adb.exhausted_budget_reports_boot_timeout_not_a_transport_error` и повторные прогоны под CPU-нагрузкой это закрепляют.

Тестовый `/bin/sleep` проверяет только lifecycle POSIX-процесса; это не Android-тест. В sandbox отсутствуют `adb`, `emulator`, AVD, Android agent и `/dev/kvm`; ни один runtime здесь не запускался.
