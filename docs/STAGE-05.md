# Этап 5: flexbox, grid, Canvas 2D, формы, cookies, хранилище

Задача этапа — сделать движок пригодным для «настоящей» страницы: современная
раскладка (flexbox и grid), рисование через `<canvas>`, интерактивные формы с
реальной отправкой GET/POST, cookies и `localStorage`/`sessionStorage`. Всё это
поверх уже существующих модулей, без новых внешних зависимостей.

Побочным результатом стала починка двух застарелых мест layout, без которых формы
не выглядели бы формами: **анонимные блоки** (inline-контент между блоками больше
не «сваливается» в конец контейнера) и **inline-block** (у контрола появился
собственный контент, отступы и рамка).

## Из чего состоит этап

```
CSS-декларации  display:flex|grid, flex-*, align-*, justify-*, gap, grid-template-*
        │
        ▼
src/layout/layout.cpp
  ├── layout_flex()          две фазы: измерение баз + распределение свободного места
  ├── layout_grid()          треки, авто-размещение, два прохода по высотам строк
  ├── measure_inline_block() inline-block как атомарный inline с собственным контентом
  └── layout_block_children() анонимные блоки: inline-прогоны в порядке документа
        │
        ▼
bindings (Canvas 2D)  fillRect/clearRect/strokeRect/fillText/beginPath/…/fill/stroke
        │             пишет в offscreen paint::Canvas → LayoutViewport::canvases
        ▼
shell::Session        canvas_bitmaps_ → layout (BoxKind::Image) → paint (blit)

HTML-формы → bindings: control.value/checked, form.elements, submit()/requestSubmit()
        │             FormSubmission{method, action, x-www-form-urlencoded body}
        ▼
shell::Session::submit_form()   событие submit (можно preventDefault)
        ├── GET  → load(action + "?" + body)
        └── POST → net::HttpRequest{method=POST, body, content-type} → load_html()
                              │
                              ▼
                     net::CookieJar  ← Set-Cookie / → Cookie: на каждый запрос
                              │
                     document.cookie (get/set, без HttpOnly)
```

## Раскладка

| Возможность | Как реализовано |
|---|---|
| `display: flex / inline-flex` | `layout_flex`: база = `flex-basis` → `width/height` → max-content (`intrinsic_width`), зажимается контейнером |
| `flex-direction` | `row`, `row-reverse`, `column`, `column-reverse` (обратный порядок детей) |
| `flex-wrap` | перенос строк по `gap`, у каждой строки свой свободный остаток |
| `flex-grow` / `flex-shrink` | свободное место делится пропорционально grow; отрицательное — по shrink |
| `justify-content` | `flex-start/end`, `center`, `space-between/around/evenly` |
| `align-items` | `flex-start`, `center`, `flex-end`, `stretch` (cross-размер через пробный layout) |
| `gap`, `row-gap`, `column-gap` | учитываются и в flex, и в grid |
| `display: grid` | `grid-template-columns/rows` (px, %, `fr`, `auto`), `repeat(n, …)`, авто-размещение row-major |
| высоты строк grid | проход 1 измеряет содержимое ячейки на её итоговой ширине, проход 2 размещает и растягивает auto-высоты |
| `min-width` / `max-width` / `min-height` | разбираются в `css` и применяются в inline-block (и `min-height` в блоках) |
| `display: inline-block` | атомарный inline: ширина = max-content/`width` (зажатая min/max), высота = пробный layout; контент раскладывается внутри content-box |
| анонимные блоки | inline-прогон между блочными детьми раскладывается на своём месте, а не в конце контейнера |
| `<br>` | жёсткий перевод строки (тот же механизм, что `\n` внутри `<pre>`) |

## Canvas 2D

Контекст живёт в `bindings`: `<canvas>` получает объект с методами
`fillRect`, `clearRect`, `strokeRect`, `fillText`, `measureText`, `beginPath`,
`moveTo`, `lineTo`, `closePath`, `rect`, `arc`, `fill`, `stroke`, `save`,
`restore`, свойствами `fillStyle`, `strokeStyle`, `lineWidth`, `globalAlpha` и
`canvas.width/height` (сброс битмапа). Растеризация — своя: заливка полигонов
по чётно-нечётному правилу (scanline), обводка штампом диска вдоль линии,
полупрозрачность через альфа-блендинг в `paint::Canvas`.

Готовый битмар отдаётся в layout через `LayoutViewport::canvases`; layout трактует
`<canvas>` как заменяемый inline (по умолчанию 300×150), paint блитит картинку.

## Формы

| Что | Поддержано |
|---|---|
| Элементы | `input` (text, password, checkbox, radio, hidden, submit, reset, button), `select`/`option`, `textarea`, `button`, `label` |
| Значения | атрибуты `value` / `checked` / `selected`; `element.value` и `element.checked` читаются и пишутся из JS |
| Отрисовка | значения «материализуются» в текстовые узлы перед раскладкой (в `select` — текст выбранной опции, в `password` — звёздочки) |
| Сбор данных | `form.elements`, `FormSubmission` с URL-кодированием (`+` для пробела, `%XX` для остального) |
| Отправка | клик по submit-кнопке, `form.submit()`, `form.requestSubmit()`; событие `submit` умеет `preventDefault()` |
| GET | action + `?` + тело запроса |
| POST | `net::HttpRequest{method=POST, Content-Type: application/x-www-form-urlencoded, Content-Length}` → ответ рендерится как новая страница |
| Локальные страницы | POST деградирует в GET (у файла нет сервера), query-строка сохраняется в адресе, но не в имени файла |

## Cookies и хранилище

* `net::CookieJar` (RFC 6265 в браузерной части): `Domain`, `Path`, `Expires`,
  `Max-Age`, `Secure`, `HttpOnly`, host-only против domain-cookie, сортировка
  «длинный путь первым», удаление по истечении.
* `document.cookie` — get/set без `HttpOnly`; на `file:` (нет origin) остаётся пустым.
* `localStorage` / `sessionStorage` — одна и та же реализация (`bindings::Storage`):
  `getItem`, `setItem`, `removeItem`, `clear`, `key`, `length`. `localStorage`
  сохраняется в `<profile>/local-storage.txt` (одна пара `ключ<TAB>значение` на
  строку с экранированием), `sessionStorage` живёт в памяти.
* Флаг CLI: `bonk view page.html --profile DIR`.

## Тесты и проверки

```
make -j && make test          → 299 тестов, 0 падений
```

Новые тесты этапа:

* `tests/layout/flex_grid_test.cpp` — 10 тестов: grow, gap + `justify-content`,
  `align-items` (center/stretch), колонка, перенос строк, `row-reverse`,
  `1fr 2fr 1fr`, явные px-треки с gap, `repeat(3, 1fr)`, разбор всех новых свойств.
* `tests/layout/inline_block_test.cpp` — inline-block на одной строке без наложений,
  `min-width`/`min-height`, `width` важнее контента, `<br>`, порядок
  «inline → block → inline», «label → input» подряд.
* `tests/bindings/forms_test.cpp` — cookies (host-only/domain, HttpOnly, Secure,
  удаление, `document.cookie`), тело `POST`-запроса на уровне байтов, сбор данных
  формы, отправка по клику (URL с query), отмена через `submit`-обработчик,
  `value`/`checked` из JS, скрытые input, `localStorage` между перезапусками.

Проверка вживую (локальный сервер, `python3` + `http.server`):

```
GET  /            → форма (input text, hidden, submit)
клик по Submit    → GET /search?q=inline+layout&src=home   (заголовок «Results…»)
                     сервер отвечает Set-Cookie → cookie сохранён в CookieJar
GET  /private     → сервер видит «Cookie: last_query=inline%20layout»
POST /posted      → сервер получает «msg=hello+server&tag=stage+five»
                     с Content-Type: application/x-www-form-urlencoded
```

Картинки: `docs/demo/flex-grid.png` (flex-навигация + `flex:1` карточки +
`1fr 2fr 1fr` сетка), `docs/demo/canvas.png` (примитивы Canvas 2D),
`docs/demo/forms.png` (форма, `form.elements`, `localStorage`).

## Что работает

* Типовая страница на flexbox/grid раскладывается правильно (карточки, навигация, сетки).
* Canvas 2D рисует прямоугольники, текст, линии и полигоны и попадает в вывод PNG.
* Формы: контролы выглядят как контролы (рамка, паддинг, `min-width`), значения
  читаются/пишутся из JS, GET и POST уходят на сервер, ответ рендерится.
* Cookies реально доезжают до сервера и возвращаются в `document.cookie`.
* `localStorage` переживает перезапуск браузера, `sessionStorage` — нет.

## Что пока не сделано

* Ввод с клавиатуры: контролы показывают значения из атрибутов, но нельзя набрать
  текст (нужен key-event → value pipeline, этап 6).
* `grid-template-areas`, `grid-column/row: span N`, `auto-fill/auto-fit`, вложенные
  grid-контейнеры внутри flex-элементов проверены только частично.
* `float`, `position: absolute/fixed`, таблицы как таблицы (`table-layout`), bidi.
* Canvas: трансформации (`translate/rotate/scale`), градиенты, `clip`, изображения
  (`drawImage`), `getImageData`; нет `requestAnimationFrame`-цикла перерисовки.
* Cookies: нет Public Suffix List и `SameSite` (это придёт вместе с этапом 6),
  нет сохранения cookies между запусками (в памяти сессии).
* Хранилище: одна «профильная» папка на все origin — разграничение по origin
  появится, когда будет Same-Origin Policy (этап 6).
* JS всё ещё без `class`, `switch`, regex, `Promise` (этап 4 — «сделано ядро»).

## Предлагаемый следующий шаг

Этап 6 — безопасность и процессы: Same-Origin Policy и CORS в `bindings`/`net`
(плюс `SameSite` и origin-скоуп для storage), CSP-базовое, индикатор HTTPS в
shell, процесс на вкладку с IPC и devtools (консоль, инспектор DOM/CSS, сеть).
Критерий выхода: кросс-доменный `fetch` без CORS блокируется, падение вкладки не
убивает браузер, devtools показывает сетевой водопад.

Затем этап 7 — конформность: html5lib-tests, test262, WPT, Acid2 и непрерывные
баг-фиксы.
