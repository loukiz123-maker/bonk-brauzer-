# Дорожная карта

Один этап за раз: каждый заканчивается работающей, проверяемой функциональностью и обновлением
`ModuleInfo::status` в `src/core/module.cpp` (после этого — `make docs`).

## Этап 0 — Фундамент ✅

**Готово**

- Workspace: `include/bonk`, `src/<module>`, `tests`, `docs`, CI.
- Сборка: `Makefile` (канонический, без зависимостей) + `CMakeLists.txt` (IDE/CI).
- Ядро: `Result/Status`, UTF-8 (WHATWG-совместимое декодирование), ASCII-строки, арена,
  диагностика, реестр модулей, версия, selftest.
- CLI `bonk` с командами `modules`, `version`, `selftest`, `help`; коды возврата 0/1/2.
- Тест-фреймворк (~180 строк) + 62 теста; ASan/UBSan чисто.
- CI: `make` × {gcc, clang} × {debug, release}, санитайзеры, `-Werror`, `cmake`+`ctest`,
  проверка актуальности генерируемой документации.

**Критерий выхода**: `make verify` зелёный на чистом клоне. ✔

## Этап 1 — net + html + dom

- `url`: парсер по WHATWG (схемы, host/IDNA-заготовка, percent-encoding, relative + base).
- `net`: DNS (`getaddrinfo`), TCP, HTTP/1.1 (chunked, keep-alive, redirects, gzip), cookies,
  кэш, стриминг тела; TLS — свой минимум (TLS 1.3: ClientHello, X25519, AES-GCM) или
  документированный системный бэкенд.
- `html`: полный токенайзер + tree builder (insertion modes, foster parenting, entities).
- `dom`: Node/Element/Text/Document, генерационные `NodeId`, атрибуты, `innerHTML`,
  `querySelector` (пока простой: tag/id/class), события capture/bubble, MutationObserver.
- CLI: `bonk fetch URL --dump-dom`, `bonk fetch URL --dump-tree`, `--diagnostics`.

**Сделано (ядро)**: `net::Url` (WHATWG-подмножество + RFC 3986), HTTP/1.1 (chunked, redirects,
таймауты), HTML-токенайзер и tree builder с неявными html/head/body и implied end tags,
DOM на индексных пулах с генерационными `NodeId`, `querySelector` с атрибутными и
псевдо-селекторами. Подробности и пробелы — [`STAGE-01-02.md`](STAGE-01-02.md).

**Сделано (2b)**: свой TLS 1.3 (ChaCha20-Poly1305 + x25519) на собственном криптоядре,
X.509 (SAN/срок/имя хоста), gzip/deflate — `https://` работает.

**Осталось до выхода с этапа**: проверка подписи сертификатов и цепочки доверия,
cookies/кэш/keep-alive, foster parenting + adoption agency, события и MutationObserver,
прогон html5lib-tests.

## Этап 2 — css + text + layout + paint

- `text`: TTF/OTF (offset table, cmap, glyf, loca, hmtx), растеризация глифов с
  antialiasing, кернинг, перенос строк, bidi-заготовка.
- `css`: токенайзер, парсер (правила, at-rules), селекторы (class/id/attr/комбинаторы/
  псевдоклассы), специфичность, каскад, наследование, единицы, `var()`, `@media`, `@font-face`.
- `layout`: box model, block/inline formatting, margin collapsing, float, positioning,
  line boxes, переполнение/скролл.
- `paint`: CPU-растеризатор (fill, borders, radius, gradient, shadow, clip), PNG-декодер
  (inflate свой), peek в JPEG/GIF/WebP, stacking contexts, вывод в PPM/PNG для проверки.
- CLI: `bonk render URL -o out.png --width 1024 --height 768`.

**Сделано (ядро)**: CSS-парсер, каскад со специфичностью и наследованием, `@media`,
computed values с `em/rem`; TrueType-парсер и растеризация глифов с антиалиасингом;
блочный и inline-layout со схлопыванием margin'ов, переносом строк и `text-align`;
программный растеризатор с альфа-смешиванием и свой PNG-энкодер.
`bonk render tests/fixtures/sample.html -o page.png` рисует страницу (см. README).

**Осталось до выхода с этапа**: декодеры PNG/JPEG/GIF/WebP для `<img>`, шейпинг и bidi,
`var()`/`calc()`, псевдоэлементы, эталонные скриншоты для регрессий.

## Этап 3 — shell

- Окно (SDL2), вкладки, адресная строка с поиском, назад/вперёд/обновить, история, закладки,
  загрузки, настройки, тёмная тема, полноэкранный режим, скролл колесом/клавиатурой, хиты по
  ссылкам и полям.

**Критерий выхода**: двумя кликами можно открыть локальную страницу, перейти по ссылке,
прокрутить её, открыть вторую вкладку.

## Этап 4 — js + bindings

- `js`: лексер, парсер в AST, tree-walking интерпретатор, объектная модель (прототипы,
  замыкания, accessor-свойства), `Object/Array/String/Number/Math/JSON/console/Error`,
  `setTimeout`. **Сделано ядро** (`src/js/{parse,interp}.cpp`, `include/bonk/js/js.hpp`).
- `bindings`: `window`, `document`, обёртки элементов (`textContent`, `innerHTML`, `classList`,
  `style`, `appendChild`, …), события с всплытием, инлайновые `<script>`.
  **Сделано ядро** (`src/bindings/bindings.cpp`).
- Впереди (этап 6–7): `class`, `switch`, regex, `Promise`/`async`, `Map/Set/Date`, байткод,
  tracing GC, `fetch`/`XHR`, микро- и макротаски; `localStorage`, Canvas 2D и формы —
  сделаны на этапе 5.

**Критерий выхода**: проходит подмножество test262 (базовый синтаксис + встроенные объекты),
страница со счётчиком на `addEventListener` и `fetch` работает.
Сейчас: счётчик на `addEventListener` работает (`tests/bindings/bindings_test.cpp`,
`docs/demo/js-demo-click.png`), test262 и `fetch` — впереди.

## Этап 5 — flexbox/grid, Canvas, формы, cookies, storage

Flexbox и Grid в `layout`, Canvas 2D в `paint`+`bindings`, формы (`input`, `select`,
`textarea`, отправка GET/POST), cookies в `net`, `localStorage`/`sessionStorage`.
Подробности: [`docs/STAGE-05.md`](STAGE-05.md), решение по состоянию —
[`docs/adr/0007-forms-cookies-storage.md`](adr/0007-forms-cookies-storage.md).

**Критерий выхода**: типовой лендинг на flexbox+grid с формой и сохранением состояния.
Сейчас: flexbox/grid раскладывают карточки и сетки (`docs/demo/flex-grid.png`), Canvas 2D
рисует примитивы (`docs/demo/canvas.png`), форма отправляется GET и POST на реальный
сервер, cookies доезжают до сервера и возвращаются в `document.cookie`,
`localStorage` переживает перезапуск (`docs/demo/forms.png`). Остаётся ввод с клавиатуры
и `grid-template-areas` — они перенесены в этап 6.

## Этап 6 — безопасность, процессы, devtools

Same-Origin Policy, CORS, CSP (базово), mixed content, индикатор HTTPS, процесс на вкладку +
IPC, devtools (консоль, инспектор DOM/CSS, сеть), профилирование и оптимизация.
Подробности: [`docs/STAGE-06.md`](STAGE-06.md), решение по изоляции —
[`docs/adr/0008-process-per-tab-ipc.md`](adr/0008-process-per-tab-ipc.md).

**Критерий выхода**: кросс-доменный `fetch` без CORS-заголовков блокируется; падение вкладки
не убивает браузер; devtools показывает сетевой waterfall.
Сейчас: cross-origin `XMLHttpRequest` без `Access-Control-Allow-Origin` блокируется
(`status 0`, `cors.allow_origin_missing`), с заголовком — читается; CSP из `<meta>` и из
HTTP-заголовка останавливает инлайн-скрипты; `bonk devtools` показывает консоль, сеть,
CSP и стили элемента; `shell::render_isolated()` рендерит вкладку в дочернем процессе и
переживает её падение (`shell.tab_crashed`). Preflight, `fetch`/`Promise`, `SameSite` и
GUI-devtools — впереди.

## Этап 7 — конформность

Прогон по внешним наборам: html5lib-tests (токенайзер и tree construction), test262,
собственные пиксельные reftests. Цель этапа — не «пройти всё», а получить измеримую
таблицу и список багов, часть которых сразу исправлена.
Подробности: [`docs/STAGE-07.md`](STAGE-07.md), отчёт — [`docs/conformance.md`](conformance.md),
решение по харнессу — [`docs/adr/0009-conformance-harness.md`](adr/0009-conformance-harness.md).

**Критерий выхода**: есть воспроизводимый прогон (`make conformance`), таблица «сколько
прошло/упало/исключено» по каждому набору и зафиксированные причины каждого исключения.
Сейчас: токенайзер — 6 284/6 823 (92.1%, ноль падений на моделируемых случаях),
tree construction — 870/1 380 (63.0%), test262 — 2 673/13 254 (20.2% в конверте движка),
reftests — 7/7. Исправлено 16 багов: полная таблица сущностей, NUL/U+FFFD, C1 →
Windows-1252, состояния комментария, script data escaped, BOM на своём слое,
`<plaintext>`, неявные `<tbody>`/`<tr>`/`<colgroup>`, foster parenting, scope для `<p>`,
высота flex-элементов, табличная раскладка. Открыто: adoption agency, `<template>`,
внешний контент SVG/MathML, fragment parsing, DOCTYPE-идентификаторы, прототипы
встроенных объектов JS, сборка мусора.

## Этап 8 — adoption agency

Список активных formatting-элементов и алгоритм adoption agency по WHATWG HTML
§13.2.6.4.7: misnested `<b>/<i>/<a>/<nobr>` больше не превращают страницу в
«лестницу». Плюс ruby-аннотации (`rb/rt/rtc/rp` как implied end tags) и режим
«in select». Подробности: [`docs/STAGE-08.md`](STAGE-08.md),
решение — [`docs/adr/0010-adoption-agency.md`](adr/0010-adoption-agency.md),
демо — [`docs/demo/adoption.png`](demo/adoption.png).

**Критерий выхода**: tree construction растёт измеримо и без регрессий в
остальных наборах; misnested-случаи совпадают с html5lib побайтово; ни одного
ASan/UBSan отчёта, `-Werror` чист. Сейчас: tree construction — 964/1 380
(69.9 %, было 63.0 %), токенайзер — 6 281/6 820 (92.1 %), test262 — 2 673/13 254
(20.2 % в конверте движка), reftests — 7/7, юнит-тестов 333. Открыто:
`<template>` (contents fragment + режим «in template»), `<frameset>`, `noscript`
со scripting, внешний контент SVG/MathML, fragment parsing, DOCTYPE-идентификаторы.


---

## Риски и как их снимаем

| Риск | Снятие |
|---|---|
| TLS «с нуля» — самая дорогая часть `net` | этап 1 даёт HTTP; TLS — отдельный подэтап 1b с тестами против локального сервера, допускается документированный системный бэкенд |
| Шрифты: TTF-парсер и hinting | начинаем с растеризации без hinting, hinting — опция; фикстуры со сравнением с эталонными bitmap'ами |
| JS-движок съедает больше всех времени | строгий порядок: парсер → интерпретатор → встроенные объекты; оптимизации только после test262-подмножества |
| Производительность layout | арена + пул layout-боксов, ре-лейаут по «грязным» поддеревьям, бенчмарк в CI с порогом |
