# Bonk Brauzer

Экспериментальный браузер с **собственным движком**: свой HTML-парсер, свой CSS-каскад, свой
layout, свой растеризатор и свой JavaScript-движок. Без Chromium/Blink, WebKit, Gecko, Servo,
V8, SpiderMonkey, QuickJS, WebView, Electron и CEF.

Зависимости: **только стандартная библиотека C++20** + системные окно/ввод (SDL2) на этапе 3.
Сегодня в проекте ровно ноль внешних зависимостей.

## Статус: Этап 0 завершён ✅

| Этап | Содержание | Статус |
|---|---|---|
| 0 | Каркас проекта, сборка, CI, контракты модулей, план | **готово** |
| 1 | `net` + `html` + `dom`: скачать страницу и напечатать дерево DOM | следующий |
| 2 | `css` + `text` + `layout` + `paint`: статичные страницы на экране | — |
| 3 | `shell`: окно, вкладки, адресная строка, клики, скролл | — |
| 4 | `js` + `bindings`: интерактивность, события, `fetch` | — |
| 5 | flexbox/grid, Canvas 2D, формы, cookies, storage | — |
| 6 | security, процесс на вкладку, devtools, оптимизация | — |
| 7 | conformance: html5lib-tests, test262, WPT, Acid2 | — |

Актуальный реестр модулей живёт в коде (`src/core/module.cpp`) и печатается бинарником:

```console
$ bonk modules
id        stage  status   depends on            title
-----------------------------------------------------------------
core      0      done     -                     Core utilities
net       1      planned  core                  Networking
html      1      planned  core                  HTML parser
...
```

## Быстрый старт

Требуется компилятор с C++20 (g++ ≥ 12 / clang ≥ 15) и GNU make. Ничего скачивать не нужно.

```console
$ make -j            # собрать build/bonk и build/bonk_tests
$ make test          # 62 юнит-теста
$ make run ARGS="modules --stage 1"
$ make verify        # тесты + selftest + проверка, что docs не устарели
$ make MODE=release -j
$ make SAN=address,undefined test     # ASan + UBSan сборка в build-address,undefined
```

CMake-путь (для IDE и CI):

```console
$ cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
$ cmake --build build-cmake -j
$ ctest --test-dir build-cmake --output-on-failure
```

### CLI

```console
$ bonk                        # краткая справка + все 8 этапов
$ bonk version --full         # Bonk Brauzer 0.1.0-stage-0 + тулчейн
$ bonk modules --stage 2      # что входит в этап 2
$ bonk modules --markdown     # таблица для docs/modules.generated.md
$ bonk selftest               # 23 быстрые проверки инвариантов рантайма
```

Коды возврата: `0` — успех, `1` — проваленная проверка, `2` — ошибка использования.

## Структура репозитория

```
include/bonk/          публичные заголовки (по модулям)
  core/                result, utf8, strings, arena, diag, module, version, selftest
  cli/                 разбор аргументов командной строки
src/                   реализации (src/<module>/<file>.cpp)
  core/  cli/  apps/   apps/main.cpp — точка входа bonk
tests/                 свой тест-фреймворк (~180 строк) + тесты по модулям
docs/                  ARCHITECTURE.md, ROADMAP.md, STAGE-00.md, ADR, generated
.github/workflows/     CI: make×{gcc,clang}×{debug,release}, ASan/UBSan, -Werror, cmake, docs
```

Правила слоёв: `core` не зависит ни от чего; `net`/`html`/`js`/`text` зависят только от `core`;
`dom` ← `html`; `css` ← `dom`; `layout` ← `css`+`text`; `paint` ← `layout`; UI (`shell`,
`devtools`) — только на верхнем уровне. Нарушение слоёв = ошибка ревью.

## Документация

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — слои, контракты модулей, модель памяти, потоки.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — этапы 0–7 с критериями готовности.
- [`docs/STAGE-00.md`](docs/STAGE-00.md) — что именно сделано на этапе 0 и как это проверить.
- [`docs/modules.generated.md`](docs/modules.generated.md) — таблица модулей из кода.
- [`docs/adr/`](docs/adr/) — архитектурные решения (ADR).

## Соглашения

- Комментарии и имена в коде — английский; документация и общение — русский.
- Ошибки: `Result<T>`/`Status`, без исключений в коде движка; ошибки разбора — в `Diagnostics`.
- Память: арена на документ + генерационные хендлы вместо сырых указателей в публичных API.
- Каждый модуль поставляется с юнит-тестами и запускаемым способом проверки результата.
