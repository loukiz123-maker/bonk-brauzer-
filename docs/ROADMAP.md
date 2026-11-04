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

**Критерий выхода**: `bonk fetch https://example.com --dump-dom` печатает дерево, совпадающее
с браузерным `document.documentElement.outerHTML` (с точностью до порядка атрибутов); 100+
фикстур html5lib-tests проходят на уровне токенайзера.

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

**Критерий выхода**: `bonk render tests/fixtures/stage2/*.html -o out.png` визуально совпадает
с эталонами; Acid1-подобная фикстура проходит.

## Этап 3 — shell

- Окно (SDL2), вкладки, адресная строка с поиском, назад/вперёд/обновить, история, закладки,
  загрузки, настройки, тёмная тема, полноэкранный режим, скролл колесом/клавиатурой, хиты по
  ссылкам и полям.

**Критерий выхода**: двумя кликами можно открыть локальную страницу, перейти по ссылке,
прокрутить её, открыть вторую вкладку.

## Этап 4 — js + bindings

- `js`: лексер, парсер ES2015+ в AST, байткод, VM, объектная модель (прототипы, замыкания,
  классы), `Promise`/`async`/генераторы, `Map/Set/JSON/RegExp/Date/Math`, mark-and-sweep GC,
  event loop (микро/макротаски, `setTimeout`, `requestAnimationFrame`).
- `bindings`: `window`, `document`, `console`, `fetch`, `XMLHttpRequest`, `localStorage`,
  History API, Canvas 2D.

**Критерий выхода**: проходит подмножество test262 (базовый синтаксис + встроенные объекты),
страница со счётчиком на `addEventListener` и `fetch` работает.

## Этап 5 — flexbox/grid, Canvas, формы, cookies, storage

Flexbox и Grid в `layout`, Canvas 2D в `paint`+`bindings`, формы (`input`, `select`,
`textarea`, отправка GET/POST), cookies в `net`, `localStorage`/`sessionStorage`.

**Критерий выхода**: типовой лендинг на flexbox+grid с формой и сохранением состояния.

## Этап 6 — безопасность, процессы, devtools

Same-Origin Policy, CORS, CSP (базово), mixed content, индикатор HTTPS, процесс на вкладку +
IPC, devtools (консоль, инспектор DOM/CSS, сеть), профилирование и оптимизация.

**Критерий выхода**: кросс-доменный `fetch` без CORS-заголовков блокируется; падение вкладки
не убивает браузер; devtools показывает сетевой waterfall.

## Этап 7 — конформность

Acid2, html5lib-tests (полностью), test262, WPT (по возможностям) и непрерывные баг-фиксы.

---

## Риски и как их снимаем

| Риск | Снятие |
|---|---|
| TLS «с нуля» — самая дорогая часть `net` | этап 1 даёт HTTP; TLS — отдельный подэтап 1b с тестами против локального сервера, допускается документированный системный бэкенд |
| Шрифты: TTF-парсер и hinting | начинаем с растеризации без hinting, hinting — опция; фикстуры со сравнением с эталонными bitmap'ами |
| JS-движок съедает больше всех времени | строгий порядок: парсер → интерпретатор → встроенные объекты; оптимизации только после test262-подмножества |
| Производительность layout | арена + пул layout-боксов, ре-лейаут по «грязным» поддеревьям, бенчмарк в CI с порогом |
