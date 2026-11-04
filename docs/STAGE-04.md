# Этап 4: собственный JavaScript-движок и DOM-биндинги

Задача этапа — исполняемый JavaScript и мост в DOM, **без V8, SpiderMonkey, QuickJS и
любых готовых VM**. Всё написано в этом репозитории: лексер, рекурсивный спуск в AST,
tree-walking интерпретатор, встроенные объекты и слой `bindings` (`window`, `document`,
элементы, события, инлайновые `<script>`).

Побочным результатом стала починка бага layout из этапа 2: инлайновый контент,
смешанный с блочным, раскладывался дважды (текст `<span>`/`<button>` после `<div>`
рисовался две копии). Теперь один inline-контекст на родителя.

## Из чего состоит этап

```
<script> в документе
      │  bindings::Bindings::run_scripts()   (по порядку документа)
      ▼
js::Context::evaluate(source, origin)
      │
      ├── parse.cpp    лексер (числа, строки, шаблоны `${}`, regex-free) + рекурсивный спуск → ast.hpp
      ├── interp.cpp   tree-walking интерпретатор: Scope-цепочки, замыкания, try/catch, this, new
      └── builtins     Object/Array/String/Number/Math/JSON/console/setTimeout/Error/parseInt
      ▼
bindings.cpp         window, document, wrapper-объекты элементов, addEventListener, bubbling
      ▼
dom::Document        appendChild / textContent / innerHTML / setAttribute / classList / style
      ▼
css → layout → paint  (пересчёт после скриптов, поэтому правки видны на экране)
```

Порядок в `shell::Session`: HTML → DOM → `js::Context` + `bindings` → **скрипты** →
сбор CSS (`<style>`, локальные `<link>`) → layout → paint. Скрипты выполняются до
стилей и раскладки, поэтому вставленные элементы сразу попадают в рендер.

## Что реализовано в языке

| Группа | Поддержано |
|---|---|
| Литералы | числа (в т.ч. `0x…`, экспонента), строки с escape и `\uXXXX`, шаблоны `` `a${b}c` ``, `true/false/null/undefined`, массивы, объекты |
| Операторы | `+ - * / % **`, `== != === !==`, `< <= > >=`, `&& \|\| ??`, `! ~`, `& \| ^ << >>`, `?:`, `= += -= *= /= %=`, `++/--`, `typeof`, `void`, `delete`, `in`, `instanceof`, `,` |
| Операторы-инструкции | `var/let/const` (блочная область для `let/const`), `if/else`, `while`, `do…while`, `for(;;)`, `for…of`, `for…in`, `break/continue`, `function`, `return`, `throw`, `try/catch/finally`, `;` (ASI по переводу строки) |
| Функции | объявления, функциональные выражения, стрелки (`x => …`, `(a,b) => …`), замыкания, рекурсия, `arguments`, `this`, `call/apply/bind`, `new` + `prototype` |
| Объекты | свойства, индексы, `__proto__`-цепочка через `prototype`, accessor-свойства (getters/setters) — на них построены DOM-обёртки |

Отдельно: `Context::evaluate` возвращает `ExecutionResult{ok, value, exception, message}`;
значение скрипта — значение последнего выражения (как в консоли браузера), ошибки
разбора помечены `js.syntax`, рантайм — `js.runtime`.

## Что реализовано в биндингах

- `window`: `document`, `location` (`href/pathname/search/hash/protocol/host`),
  `navigator.userAgent`, `innerWidth/innerHeight`, `addEventListener`, `matchMedia`.
- `document`: `getElementById`, `querySelector(All)`, `getElementsByTagName`,
  `getElementsByClassName`, `createElement`, `createTextNode`, `title` (чтение/запись),
  `body`, `documentElement`, `readyState`, `URL`.
- Элемент: `id`, `className`, `classList` (`add/remove/toggle/contains`), `textContent`,
  `innerHTML` (реальный разбор фрагмента нашим HTML-парсером), `outerHTML`,
  `get/set/has/removeAttribute`, `style` (accessor-свойства + `setProperty`),
  `children`, `parentNode`, `nextElementSibling`, `appendChild`, `removeChild`,
  `querySelector(All)`, `matches`, `click`.
- События: `addEventListener`/`removeEventListener`/`dispatchEvent`, всплытие от цели до
  `document`, `event.type/target/currentTarget/preventDefault()`.
  `Session::click(x, y)` сначала стреляет `click` в DOM и пересчитывает layout; если
  обработчик вызвал `preventDefault()`, переход по ссылке не выполняется.

## Как проверить

```console
$ make -j && make test                      # 271 тест, из них 46 на js и 16 на bindings
$ ./build/bonk js 'var s = 0; for (var i = 1; i <= 10; i++) s += i; s'
55
$ ./build/bonk js 'console.log("hi", 1 + 1)'
log: hi 2
$ ./build/bonk view docs/demo/js.html --screenshot docs/demo/js-demo.png --width 640 --height 420
$ ./build/bonk view docs/demo/js.html --screenshot docs/demo/js-demo-click.png \
      --width 640 --height 420 --click 57,286
```

`docs/demo/js.html` — страница, которая целиком строится скриптом: заголовок, три карточки
через `createElement/appendChild`, счётчик кликов через `addEventListener`.
Первый скриншот — состояние после загрузки, второй — после клика по кнопке
(`clicks: 1`). Это проверка полного цикла: JS → DOM → CSS → layout → paint.

Живая проверка на настоящем сайте:

```console
$ ./build/bonk view https://github.com/ --screenshot docs/github.png --width 900 --height 620
viewed https://github.com/ -> docs/github.png (900x620, ... nodes=2858)
```

## Тесты

- `tests/js/lexer_test.cpp` — числа, escape-последовательности, комментарии, шаблонные
  строки, ASI, диагностика незакрытых строк/шаблонов/комментариев.
- `tests/js/parser_test.cpp` — приоритеты операторов, право-ассоциативное `=`, стрелки,
  литералы, отказ на `switch`/`class` с внятным сообщением, глубокая вложенность выражений.
- `tests/js/interpreter_test.cpp` — замыкания, рекурсия, циклы, `for…of`/`for…in`,
  `try/catch/finally`, `this`, `new` + `prototype`, `instanceof`, приведения типов, `delete`/`in`.
- `tests/js/builtins_test.cpp` — `console`, `Math`, `JSON`, `parseInt/parseFloat`,
  методы массивов/строк, `toFixed`, `Error`, таймеры (`setTimeout` + `run_pending_callbacks`),
  `call/apply`, инъекция хостовых глобалов.
- `tests/bindings/bindings_test.cpp` — идентичность обёрток, write-through аксессоров,
  `createElement/appendChild`, `innerHTML`, `classList`, `style`, `document.title`,
  селекторы, всплытие кликов, `preventDefault`, `element.click()`, порядок и отказы скриптов.

Всего в проекте: **271 тест, 0 падений** (проверено в debug, в release с
`-Wall -Wextra -Wpedantic -Wshadow -Werror` и под ASan+UBSan).

## Чего пока нет (осознанно, с планом)

| Ограничение | Почему / когда |
|---|---|
| `class`, `switch`, `async/await`, генераторы, деструктуризация, spread | не нужны для демо-страниц; `class`/`switch` дают явную ошибку `js.syntax`, а не тихий сбой. Классы — этап 5–7 |
| Регулярные выражения | `/` всегда деление; regex-литерал — отдельный парсер и backtracking-движок, это этап 7 (test262) |
| `eval`, `with`, `getter/setter` в литералах объектов | сознательно вне подмножества |
| Сборка мусора | значения на `shared_ptr`, циклы в пользовательских объектах утекают; tracing GC — этап 6 |
| Внешние `<script src>` | нет загрузчика скриптов; такие теги пропускаются с записью в `script_runs()` |
| `fetch`, `XMLHttpRequest`, `localStorage`, Canvas 2D | этап 5–6 (`bindings` + `net`) |
| DOM-мутации из JS не уведомляют style/layout напрямую | проще и честнее: сессия пересобирает CSS/layout после скриптов и после клика |
| Нет `requestAnimationFrame`, микротасок, `Promise` | этап 5 (event loop) |
| `document.write`, парсинг во время загрузки | нет инкрементального парсера — этап 7 |

## Что дальше

Этап 5: flexbox/grid в `layout`, Canvas 2D, формы и валидация, cookies/`localStorage`,
и только после них — `security` (SOP/CORS/CSP) и DevTools на этапе 6.
