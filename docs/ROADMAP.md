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


## Этап 9 — `<template>`, `<frameset>`, границы scope

Содержимое `<template>` переехало в настоящий `DocumentFragment`
(`NodeType::Fragment`), появился упрощённый режим «in template». `<frameset>`
заменяет ещё пустой `<body>` по флагу frameset-ok, а закрывающие теги больше не
выходят за scope-границы (`template`, `table`, `td`, список для `li/dd/dt`).
Подробности: [`docs/STAGE-09.md`](STAGE-09.md),
решение — [`docs/adr/0011-template-contents-and-frameset.md`](adr/0011-template-contents-and-frameset.md),
демо — [`docs/demo/structure.png`](demo/structure.png).

**Критерий выхода**: tree construction растёт измеримо, содержимое шаблона не
попадает в layout, frameset-документы разбираются, регрессий в остальных наборах
нет. Сейчас: tree construction — 1 092/1 380 (79.1 %, было 69.9 %), токенайзер —
6 281/6 820 (92.1 %), test262 — 2 673/13 254 (20.2 % в конверте движка),
reftests — 7/7, юнит-тестов 339. Открыто: полный автомат insertion mode (таблица
внутри `<template>`), foster parenting в сложных комбинациях, `noscript` со
scripting, `<menuitem>`, комментарии `--!`, DOCTYPE-идентификаторы, рендеринг
frameset'ов, внешний контент SVG/MathML, fragment parsing.


## Этап 10 — insertion modes

Парсер переведён с заплаток «вокруг таблиц» на режимы вставки: текущий узел
стека (`table`/`tbody`/`tr`/`td`/`colgroup`) выбирает набор правил, обработчики
возвращают «токен забран» и проваливаются по цепочке (TableBody → Table, Row →
TableBody → Table). Удалены `kTableAllowed` и блок table-fixes. Плюс: закрывающие
теги до `<body>` игнорируются, «after frameset» работает по правилам.
Подробности: [`docs/STAGE-10.md`](STAGE-10.md),
решение — [`docs/adr/0012-insertion-modes.md`](adr/0012-insertion-modes.md),
демо — [`docs/demo/table.png`](demo/table.png).

**Критерий выхода**: табличные конструкции строятся по правилам режимов, старые
заплатки удалены, регрессий нет. Сейчас: tree construction — 1 110/1 380
(80.4 %, было 79.1 %), токенайзер — 6 281/6 820 (92.1 %), test262 — 2 673/13 254
(20.2 %), reftests — 7/7, юнит-тестов 343. Открыто: порядок foster parenting и
слияние текста перед таблицей, «in template» с переключением режимов, `noscript`
со scripting, `<menuitem>`, `--!`, DOCTYPE-идентификаторы, рендеринг frameset'ов,
внешний контент SVG/MathML, fragment parsing.


## Этап 11 — foster parenting и режимы в `<template>`

Foster parenting стал свойством места вставки (`insert_node`/`insert_text` и
`foster_target()`), а не флагом режима: `<table><div>x</div></table>` больше не
перевешивает текст из `<div>`. Текст перед таблицей сливается в один узел
(`Document::extend_text`), formatting-элементы внутри таблицы реконструируются,
adoption agency уважает перевешивание. Внутри `<template>` работает стек
режимов (`template_modes_`). Подробности: [`docs/STAGE-11.md`](STAGE-11.md),
решение — [`docs/adr/0013-foster-parenting-and-template-modes.md`](adr/0013-foster-parenting-and-template-modes.md).

**Критерий выхода**: перевешивание ведёт себя как в стандарте, «in template»
переключает режимы, регрессий нет. Сейчас: tree construction — 1 129/1 380
(81.8 %, было 80.4 %), токенайзер — 6 281/6 820 (92.1 %), test262 — 2 673/13 254
(20.2 %), reftests — 7/7, юнит-тестов 348. Открыто: полный набор «in body»
правил (`<h1>` в `<h1>`, `<menuitem>`, `</br>`), `noscript` со scripting,
`plaintext`, DOCTYPE-идентификаторы, `--!`, рендеринг frameset'ов, внешний
контент SVG/MathML, fragment parsing.


## Этап 12 — table scope и «in body» правила

Табличные режимы переведены на **table scope** (границы `html`/`table`/`template`),
поэтому `</table>` из ячейки закрывает всё до таблицы, а токены вне scope
игнорируются. Правила заголовков (`<h1>` закрывает только текущий заголовок),
`<button>`, `</li>`/`</dd>`/`</dt>` и `<search>` реализованы по стандарту.
NUL теперь пропускает токенайзер, а дерево-строитель его игнорирует (в таблице —
U+FFFD). Подробности: [`docs/STAGE-12.md`](STAGE-12.md),
решение — [`docs/adr/0014-table-scope-and-nul.md`](adr/0014-table-scope-and-nul.md).

**Критерий выхода**: scope-проверки совпадают со стандартом, NUL обрабатывается
на своём слое, регрессий нет. Сейчас: tree construction — 1 153/1 380 (83.6 %,
было 81.8 %), токенайзер — 6 281/6 820 (92.1 %), test262 — 2 673/13 254 (20.2 %),
reftests — 7/7, юнит-тестов 353. Открыто: `template`/`frameset` комбинации,
`noscript` со scripting, `<select>`-крайности, `plaintext`/CR, DOCTYPE-идентификаторы,
рендеринг frameset'ов, внешний контент SVG/MathML, fragment parsing.


## Этап 13 — fragment parsing (`Element.innerHTML`)

Реализован алгоритм фрагментного разбора: контекстный элемент **не лежит в стеке**
открытых элементов (в стеке только поддельный `<html>`, `current()` отдаёт
контекст), режим вставки берётся из тега контекста по «reset the insertion mode»
с оговоркой `last = true`, а текстовые контексты (`title`/`textarea`/`script`/…)
входят в RCDATA/RAWTEXT без «appropriate end tag». Корпус начал считать 155
фрагментных случаев. `pop_through()` больше не снимает корневой `<html>` и
сообщает, снял ли он элемент, — из-за этого ушли циклы переразбора в табличных
режимах. `innerHTML` в bindings переведён на `parse_fragment` с тегом самого узла.
Подробности: [`docs/STAGE-13.md`](STAGE-13.md), решение —
[`docs/adr/0015-fragment-parsing.md`](adr/0015-fragment-parsing.md).

**Критерий выхода**: фрагменты разбираются контекстно, `innerHTML` работает как в
браузере, регрессий нет. Сейчас: tree construction — 1 340/1 535 (87.3 %, было
83.6 %; фрагментных 149/155), токенайзер — 6 281/6 820 (92.1 %), test262 —
2 673/13 254 (20.2 %), reftests — 7/7, юнит-тестов 369. Открыто: adoption agency
для `<aside>`-случаев, `template`/`frameset`, `noscript` со scripting,
`<select>`-крайности, `plaintext`/CR, DOCTYPE-идентификаторы, внешний контент
SVG/MathML, рендеринг frameset'ов.


## Этап 14 — frameset, `noscript` со scripting и правила head

Флаг `frameset-ok` переписан как список тегов, которые его **сбрасывают**
(`pre`, `br`, `table`, `input` без `type=hidden`, …), поэтому `<div><frameset>`
даёт frameset-документ, а `<br><frameset>` — `<body>`. `<noscript>` стал двумя
элементами: RAWTEXT при включённом scripting и контейнер с режимом «in head
noscript» при выключенном (раннер теперь читает `#script-on`/`#script-off`).
Добавлены отбрасывание первого LF в `pre`/`listing`/`textarea`, `<image>` → `<img>`,
регистронезависимый `type=hidden`, указатель на открытый `<form>`, `plaintext`
в любом режиме, «in select in table», `</p>` по button scope и режимы
«after body»/«after frameset» для комментариев. Подробности:
[`docs/STAGE-14.md`](STAGE-14.md), решение —
[`docs/adr/0016-frameset-ok-and-noscript.md`](adr/0016-frameset-ok-and-noscript.md).

**Критерий выхода**: решения о типе документа совпадают со стандартом,
`noscript`/`pre` ведут себя как в браузере, регрессий нет. Сейчас: tree
construction — 1 446/1 535 (94.2 %, было 87.3 %), токенайзер — 6 281/6 820
(92.1 %), test262 — 2 673/13 254 (20.2 %), reftests — 7/7, юнит-тестов 379.
Открыто: adoption agency (furthest block, `<marquee>`/`<nobr>`), `template`/
`frameset`, caption-режим, внешний контент SVG/MathML, DOCTYPE-идентификаторы,
рендеринг frameset'ов.


## Этап 15 — `<select>` без режима, adoption agency и пробелы в head

Внутренний цикл adoption agency приведён к порядку стандарта: узел, до которого
осталось больше четырёх шагов, выбрасывается из стека, а не клонируется, —
поэтому `<b><em><foo>×10<aside></b>` даёт правильное дерево, а `common ancestor`
во фрагменте указывает на контекстный элемент. `<select>` перестал быть режимом
вставки: `option`/`optgroup`/`hr`/`select`/`input` остались особыми правилами,
всё остальное (`<div>`, `<button>`, `<datalist>`, `<b>`, …) разбирается «in body».
Пробельные прогоны токенов обрабатываются отдельными шагами (head/after head,
frameset, after body), `<html>`/`<body>` внутри `<template>` игнорируются,
комментарий попадает в `template.content`, плюс quirks-`<table>`, маркер
`<marquee>` и foster-`</p>`. Подробности: [`docs/STAGE-15.md`](STAGE-15.md),
решение — [`docs/adr/0017-select-adoption-agency-and-whitespace.md`](adr/0017-select-adoption-agency-and-whitespace.md).

**Критерий выхода**: дерево совпадает с html5lib на всех фрагментах и на
«обычных» документах, регрессий нет. Сейчас: tree construction —
1 519/1 535 (99.0 %, было 94.2 %; фрагментных 155/155), токенайзер —
6 281/6 820 (92.1 %), test262 — 2 673/13 254 (20.2 %), reftests — 7/7,
юнит-тестов 385. Открыто: template-таблицы, `<selectedcontent>`, caption-режим,
«Noah's Ark» с атрибутами, foster + formatting-элементы, внешний контент
SVG/MathML, DOCTYPE-идентификаторы, рендеринг frameset'ов.


---

## Риски и как их снимаем

| Риск | Снятие |
|---|---|
| TLS «с нуля» — самая дорогая часть `net` | этап 1 даёт HTTP; TLS — отдельный подэтап 1b с тестами против локального сервера, допускается документированный системный бэкенд |
| Шрифты: TTF-парсер и hinting | начинаем с растеризации без hinting, hinting — опция; фикстуры со сравнением с эталонными bitmap'ами |
| JS-движок съедает больше всех времени | строгий порядок: парсер → интерпретатор → встроенные объекты; оптимизации только после test262-подмножества |
| Производительность layout | арена + пул layout-боксов, ре-лейаут по «грязным» поддеревьям, бенчмарк в CI с порогом |
