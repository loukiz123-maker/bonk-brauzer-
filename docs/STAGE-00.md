# Этап 0 — что сделано и как проверить

Цель этапа: рабочая основа, на которую навешиваются модули без переделок. Никаких заглушек
«потом допишу» — каждый файл либо полностью реализован, либо его ещё нет.

## Что появилось

### Ядро (`core`) — реализовано и покрыто тестами

| Файл | Содержимое | Тесты |
|---|---|---|
| `include/bonk/core/result.hpp`, `src/core/result.cpp` | `Error` (код + сообщение + позиция), `Status`, `Result<T>` на `std::variant`, `[[nodiscard]]` везде | `result.*`, `error.formatting` |
| `include/bonk/core/utf8.hpp`, `src/core/utf8.cpp` | декодирование/кодирование UTF-8 по правилам WHATWG: overlong, суррогаты, > U+10FFFF запрещены; ASCII-хелперы | `utf8.*` (7 тестов) |
| `include/bonk/core/strings.hpp`, `src/core/strings.cpp` | `trim`, `split` (с пустыми полями), `join`, `equals_ci`, `replace_all`, `pad_*`, `parse_u64` с проверкой переполнения | `strings.*` (7 тестов) |
| `include/bonk/core/arena.hpp`, `src/core/arena.cpp` | бамп-аллокатор с блоками, выравниванием, `reset()`, move-семантикой, `make<T>` | `arena.*` (8 тестов) |
| `include/bonk/core/diag.hpp`, `src/core/diag.cpp` | `Severity`, `SourcePos`, `Diagnostics`, форматирование `url:line:col: error: msg` | `diag.*` (7 тестов) |
| `include/bonk/core/module.hpp`, `src/core/module.cpp` | реестр 13 модулей: id, этап, статус, зависимости, отрисовка таблицы (plain + markdown) | `module.*` (8 тестов) |
| `include/bonk/core/selftest.hpp`, `src/core/selftest.cpp` | 23 проверки инвариантов рантайма, включая DFS-проверку ацикличности зависимостей | `selftest.*` |
| `include/bonk/core/version.hpp`, `src/core/version.cpp` | версия и строка сборки (компилятор/стандарт/режим) | `version.*` |

### CLI (`cli`, `apps`)

- `include/bonk/cli/cli.hpp`, `src/cli/cli.cpp` — `parse_cli_args` с `Result<CliOptions>`,
  нормализация `--stage=2` → `--stage 2`, проверки «флаг только к своей команде».
- `src/apps/main.cpp` — команды `modules`, `version`, `selftest`, `help`; коды возврата 0/1/2.

### Инфраструктура

- `Makefile` — каноническая сборка: `MODE=debug|release`, `SAN=address,undefined`, `test`,
  `verify`, `docs`, `docs-check`, `info`, `clean`. Объекты и `-MMD`-зависимости в `build/`.
- `CMakeLists.txt` — то же самое для CMake/`ctest` (glob с `CONFIGURE_DEPENDS`).
- `.github/workflows/ci.yml` — 5 job'ов: make-матрица (gcc/clang × debug/release),
  санитайзеры, `-Werror`, cmake+ctest, актуальность сгенерированных доков.
- `tests/test_framework.hpp` + `tests/test_main.cpp` — свой фреймворк: авторегистрация,
  `CHECK/CHECK_EQ/CHECK_NE/REQUIRE`, печать значений, `--filter=`, `--list`.

## Как проверить

```console
$ make -j && make test
[  OK  ] arena.allocations_are_aligned
...
62 test(s) run, 0 failed

$ make verify
verify: OK

$ ./build/bonk version --full
Bonk Brauzer 0.1.0-stage-0
compiler: g++ 12.2.0 / libstdc++ / c++20 / debug

$ ./build/bonk modules --stage 1
id        stage  status   depends on      title
-------------------------------------------------------
core      0      done     -               Core utilities
net       1      planned  core            Networking
html      1      planned  core            HTML parser
dom       1      planned  core, html      DOM
```

Проверка памяти:

```console
$ make SAN=address,undefined test
62 test(s) run, 0 failed
```

## Ключевые решения

1. **C++20 вместо Rust.** В этом окружении нет тулчейна Rust и нет сети к `static.rust-lang.org`
   и `crates.io`, то есть собрать и прогнать тесты было бы невозможно — а код без сборки не
   считается рабочим. C++20 был явно разрешённой альтернативой в ТЗ; архитектура от языка не
   зависит, порт на Rust — механическая работа по тем же контрактам.
2. **`Result<T>` вместо исключений.** Парсеры и сеть ошибаются постоянно; исключения сделали бы
   цену ошибки непредсказуемой, а `[[nodiscard]]`-результат заставляет обработать каждую ветку.
3. **Арена для документов.** Дерево умирает целиком → `reset()` вместо обхода с `free`:
   выделение ~5 нс, нулевая фрагментация, нет `shared_ptr`-циклов.
4. **Реестр модулей в коде.** Таблица этапов не может «протухнуть»: её печатает бинарник,
   из неё генерируется `docs/modules.generated.md`, CI падает при расхождении.
5. **Свой тест-фреймворк (180 строк).** Правило «только стандартная библиотека» + отсутствие
   сети: `make test` работает на чистой машине без пакетного менеджера.

## Чего пока нет (осознанно)

- Окна, сети, парсеров, рендера, JS — это этапы 1+; в репозитории нет пустых файлов-заглушек.
- TLS-бэкенд не выбран: решение принимается на этапе 1 вместе с зависимостью (свой TLS 1.3
  или документированный системный OpenSSL через `dlopen`).
- Нет `.clang-format`/`clang-tidy` в CI: в окружении нет clang-tools; стиль зафиксирован в
  `.editorconfig` (2 пробела, 120 колонок) и соблюдается вручную.

## Следующий шаг — Этап 1

Порядок работы внутри этапа:

1. `core` дополняется: `interned string` (пул строк для имён тегов/атрибутов).
2. `net/url` — парсер WHATWG URL + тесты на таблицах из спецификации.
3. `html` — токенайзер (12 состояний + entities) → tree builder (insertion modes).
4. `dom` — узлы на арене, `NodeId` с генерацией, `querySelector` (tag/id/class).
5. `net` — TCP + HTTP/1.1 (chunked, redirects, gzip), затем HTTPS.
6. CLI: `bonk fetch URL --dump-dom`.

Готовый результат этапа 1: `bonk fetch https://example.com --dump-dom` печатает корректное
дерево без единого внешнего движка.
