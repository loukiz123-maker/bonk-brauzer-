# Уведомление агентам разработки от QA-агента

Дата: 2026-11-10. Ветка QA: `arena/640564c5-bonk-brauzer`.
Полный отчёт: `qa/REPORT-2026-11-10.md`.

## Промпт (скопируйте агенту как есть)

> В проекте работает независимый QA-агент. Он проверяет ваш код и **сам
> исправляет найденные ошибки**. Исправления лежат в ветке
> `arena/640564c5-bonk-brauzer`.
>
> 1. **Не исправляйте повторно** ошибки из списка ниже и не переписывайте
>    исправленные участки. Возьмите готовые исправления (команды ниже).
> 2. Если вам нужно изменить строки, которые исправил QA-агент, — не
>    откатывайте исправление и не удаляйте его регрессионный тест. Сначала
>    опишите, что и зачем меняете; тест должен продолжать проходить.
> 3. Регрессионные тесты QA (`tests/gui/test_launcher_qa_regressions.py`,
>    тесты `QA regressions` в `tests/browser/preview.test.cjs`,
>    `TestAgentDoesNotTouchFiles`, `png.decodes_adam7_images_that_use_all_seven_passes`,
>    `android_engine.*process_group*`) не удалять, не ослаблять и не помечать skip.
> 4. Пункты из раздела «Открыто — решение за владельцем» QA-агент **не**
>    исправлял: это ваша зона или решение пользователя.
> 5. Не выдавайте тесты на заглушках (fake ADB, fake Waydroid CLI, offscreen Qt,
>    «тест без Android») за доказательство работы Android или игр. Реальный
>    Android в этом репозитории пока ни разу не запускался.
> 6. В `main` не пушить; объединение веток согласует пользователь.
> 7. **Решение владельца проекта от 2026-11-10: Waydroid убрать из движка.**
>    Агент движка: примените патч удаления (команды ниже) и не возвращайте
>    Waydroid — ни адаптер, ни команду `bonk waydroid`, ни под другим именем.
>    Runtime backend теперь не выбран; новый backend не начинайте без
>    согласования с владельцем. Про CLI лаунчера решения пока нет — его не
>    трогать.

## Агенту движка (ветка `arena/906ff810-bonk-brauzer`)

### 1. Удаление Waydroid (решение владельца, обязательно к применению)

Патч `qa/patches/engine/0001-android-remove-the-Waydroid-runtime-and-bonk-waydroi.patch`,
база — ваш `23b78b2`.

Удалено:

- `include/bonk/android/waydroid_runtime.hpp`, `src/android_engine/waydroid_runtime.cpp`,
  `tests/android_engine/waydroid_runtime_test.cpp`;
- команда `bonk waydroid` и флаги `--waydroid`, `--confirm-start`, `--show-ui`,
  `--install-apk`, `--confirm-install-apk`, `--launch-package`
  (`src/apps/main.cpp`, `src/cli/cli.cpp`, `include/bonk/cli/cli.hpp`,
  `tests/cli/cli_test.cpp`, `Makefile` возвращены к состоянию `fa22179`);
- `tests/waydroid_cli_test.sh` и цель `make waydroid-cli-test`;
- runbook Waydroid и все утверждения «Waydroid — выбранный runtime» в
  `docs/ANDROID_ENGINE.md`, `docs/LYNXDROID_ARCHITECTURE.md`.

Сохранено (ваша работа не выброшена):

- `engine_state` и `launcher_plan` со всеми тестами. Они зависели от Waydroid
  только именами типов: `WaydroidState`/`WaydroidStatus` заменены на
  `RuntimeState`/`RuntimeStatus` из нового `include/bonk/android/runtime_status.hpp`.
- Коды ошибок переименованы: `android.waydroid_start_consent_required` →
  `android.runtime_start_consent_required`, `android.waydroid_state_unknown` →
  `android.runtime_state_unknown`.
- `EngineSession`, `make_emulator_command`, preflight, `adb_client`,
  `input_action` не тронуты.

Что из этого следует — пишите об этом честно: **runtime backend не выбран, в
дереве нет кода, который запускает Android**; `plan_launcher()` и
`render_engine_snapshot()` — библиотечный код с unit-тестами без вызывающей
стороны. Новый тест `cli.has_no_waydroid_command_or_options` падает, если
команда или любой из её флагов вернётся; не удалять и не ослаблять.

### 2. Исправления ошибок

Проверено поверх вашего `23b78b2` вместе с патчем удаления Waydroid
(`make verify` — 368 тестов, 0 ошибок; ASan/UBSan — 368/0; `cmake + ctest` —
100 %; один патч удаления без исправлений — 364/0, сборка с `-Werror` чистая):

| Что | Файлы | Коммит |
|---|---|---|
| Adam7: пятый проход имел шаг строк 2 вместо 4 — любой interlaced PNG выше 6 строк отклонялся | `src/image/png.cpp`, `tests/image/png_test.cpp` | `fcc1c62` |
| `ctest` запускал тесты из каталога сборки, 8 тестов падали, задача CI «cmake + ctest» была красной на каждом коммите `main` и вашей ветки | `CMakeLists.txt` | `fcc1c62` |
| `EngineSession::stop()` и деструктор сообщали «stopped», оставляя живыми остальные процессы группы | `src/android_engine/engine_session.cpp`, `include/bonk/android/engine_session.hpp`, `tests/android_engine/engine_session_test.cpp` | `b381c61` |

Как взять всё сразу (патч и коммиты не пересекаются по файлам, конфликтов нет):

```sh
git fetch origin arena/640564c5-bonk-brauzer
for p in $(git ls-tree --name-only FETCH_HEAD qa/patches/engine/ | sort); do
  git show "FETCH_HEAD:$p" | git am -3 || break
done
git cherry-pick fcc1c62 b381c61
make -j2 verify        # ожидается: 368 test(s) run, 0 failed; verify: OK
git grep -il waydroid  # только docs (запись об удалении), launcher_plan.hpp,
                       # runtime_status.hpp, tests/cli/cli_test.cpp
```

Изменение поведения, о котором нужно знать: после выхода лидера `stop()`
сразу посылает SIGKILL остаткам группы (время `grace_period` даётся самому
runtime-процессу). Если помощникам нужно время на завершение после лидера —
это архитектурное решение за вами, но тест «никто из группы не выживает после
успешного stop()» должен остаться зелёным.

Открыто — решение за владельцем (QA не менял):

- Если runtime завершился сам (не через `stop()`), оставшиеся члены его группы
  процессов не отслеживаются и не останавливаются.
- **Выбор runtime backend (без Waydroid).** После удаления движок не умеет
  запускать Android ничем, кроме построителя команды для SDK Emulator/AVD
  (необязательный тестовый backend, игровым runtime не объявлен). Какой backend
  делать — решает владелец; QA его не выбирал и не писал.
- Код `engine_state`, `input_action`, `launcher_plan` (коммиты
  `0c35958..23b78b2`) QA проверил сборкой, тестами и санитайзерами; построчно
  прочитаны только `engine_state` и `launcher_plan` (при замене типов),
  `input_action` и новые части `adb_client` — нет.
- В вашем `docs/LYNXDROID_ARCHITECTURE.md` за QA записаны «profile
  validation/persistence» и «licensing metadata». Таких правок QA **не делал**:
  ошибок в сохранении профилей не найдено, а тексты лицензий ждут решения
  пользователя. Исправьте формулировку, чтобы она не обещала несуществующих патчей.

## Агенту лаунчера (ветка `arena/2dcca4f1-bonk-brauzer`, PR #2)

У вашей ветки нет общей истории с `main`, поэтому исправления переданы
патчами. Проверено поверх вашего `4c0648f`: `git am` без конфликтов; ruff,
формат, строгий mypy — чисто; pytest — 1018 passed, 1 skipped; браузерные тесты
— 21/21.

```sh
git fetch origin arena/640564c5-bonk-brauzer
for p in $(git ls-tree --name-only FETCH_HEAD qa/patches/launcher/ | sort); do
  git show "FETCH_HEAD:$p" | git am || break
done
ruff check . && ruff format --check . && mypy && pytest -q
```

| Патч | Что исправлено |
|---|---|
| `0001` | Qt: залипание перетаскивания после потери фокуса/потерянного отпускания кнопки; привязки и «тест без Android» не работали при русской раскладке; выход из полного экрана сбрасывал развёрнутое окно в обычное; подсветка навигации переключалась при отказе сбросить несохранённое; подпись шире боковой панели вызывала горизонтальную прокрутку. +20 тестов |
| `0002` | Веб-предпросмотр: то же с раскладкой клавиатуры (`event.key` → запасной `event.code`); импорт раскладки во время загрузки/сохранения профиля; гонка в самих браузерных тестах (локально 15/19 → 21/21). +2 теста |
| `0003` | `TouchAgentService` удалял произвольный файл по пути из Intent. +1 тест (проверка исходника, агент не собирался) |
| `0004` | Числа тестов в README/TESTING/ROADMAP. Если не применяется из-за ваших новых тестов — пропустите (`git am --skip`) и поставьте числа из `pytest --collect-only -q` |

Открыто — решение за владельцем (QA не менял):

- `lynxdroid doctor` завершается `BLOCKED` без Waydroid; `lynxdroid version`
  печатает «stage 4 - architecture and backend contract»; в пакете остаются
  `backend/waydroid.py` и старый GUI (`main_window.py`, `viewmodel.py`,
  `workers.py`). GUI-лаунчер от Waydroid не зависит, CLI — зависит.
- `LICENSE` и `docs/LICENSES.md` описывают Waydroid как зависимость и не
  упоминают заимствование из scrcpy (файл лицензии при этом в пакете есть).
- Происхождение `data/desktop-bg.jpg` и `data/launcher-bg.jpg` не указано;
  `launcher-bg.jpg` кодом не используется, но попадает в wheel.
- `TouchAgentService` не проверяет, кто подключился к сокету
  (`getPeerCredentials`): при наличии `INJECT_EVENTS` любое приложение на
  устройстве могло бы слать касания. Клиент ждёт файловый сокет, агент
  создаёт абстрактный — стороны не соединяются.
- Повреждённый профиль открывается «только для чтения», но холст
  редактируется, а сбросить профиль из интерфейса нельзя.
- После удаления игры её профиль `control-profiles/<sha256>.json` остаётся.
- Некорректный джойстик и дубли клавиш отклоняются только кнопкой «Проверить».
- Глифы `⛶`, `ⓘ`, `＋` отсутствуют в DejaVu (пустые квадраты, если в системе
  нет других шрифтов); у `◆ LYNXDROID` в шапке Qt виден более тёмный фон.
- `tests/browser/node_modules/` не в `.gitignore`; sdist не содержит `tests/`,
  `docs/`, `web-preview/`.
- Предпросмотр и лаунчер расходятся во внешнем виде (высота шапки 68/60 px,
  фон и подсказка холста, подписи полей, отключённые кнопки окна) — см. отчёт.
