# Этап 11: foster parenting по правилам и режимы внутри `<template>`

Этап 10 дал режимы вставки, но оставил два известных долга, оба записанных в
отчёте честно: «порядок foster parenting и слияние текста перед таблицей»
(~50 случаев) и «`<template>` должен переключать режим на табличные» (29 случаев).
Этот этап закрывает оба — и по дороге выясняется, что первый долг был не про
порядок, а про **место вставки**.

## Результат в цифрах

| набор | этап 10 | этап 11 | Δ |
|---|---|---|---|
| html5lib tree-construction | 1 110 / 1 380 = 80.4 % | **1 129 / 1 380 = 81.8 %** | +19 случаев |
| html5lib tokenizer | 6 281 / 6 820 = 92.1 % | 6 281 / 6 820 = 92.1 % | не трогали |
| test262 | 2 673 / 13 254 = 20.2 % | 2 673 / 13 254 = 20.2 % | не трогали |
| reftests | 7 / 7 | 7 / 7 | не трогали |
| собственные юнит-тесты | 343 | **348** | +5 |

## Что сделано

### 1. Foster parenting — это свойство *места вставки*, а не режима

Раньше «перевесить перед таблицей» было флагом режима: в табличных режимах мы
звали `foster_insert`. Оказалось, стандарт формулирует иначе — есть одна функция
«appropriate place for inserting a node», и перевешивание включается только если
**цель вставки** — `table`/`tbody`/`tfoot`/`thead`/`tr`:

```cpp
[[nodiscard]] bool foster_target() const {
  const std::string_view top = tag_of(current());
  return top == "table" || top == "tbody" || top == "tfoot" || top == "thead" || top == "tr";
}
void insert_node(dom::NodeId node) { foster_target() ? foster_insert(node)
                                                     : document_.append_child(insertion_parent(), node); }
```

Из этого сразу следует правильное поведение `<table><div>x</div></table>`:
`<div>` перевешивается (его цель — таблица), а текст `x` уже нет — его цель
`<div>`. Раньше мы перевешивали и текст, получая `<div></div>x`.

### 2. Текст, перевешенный перед таблицей, сливается с соседом

`Document::append_text(parent, ...)` добавляет **ребёнка**-текст, поэтому
«дописать в существующий текстовый узел» им нельзя (текст оказывался внутри
текстового узла и исчезал из сериализации). Появился `Document::extend_text()`,
и `foster_text()` теперь дописывает в узел непосредственно перед таблицей:
`<table>A<td>B</td>C</table>` → один узел `"AC"`, как в корпусе.

### 3. Форматирование внутри таблицы идёт через AFE

`<table><a>1<td>2</td>3</table>` — канонический случай, где нужны оба списка:
`<a>` перевешивается, но остаётся активным; при закрытии ячейки он уходит из
стека, а на символе `3` **реконструируется** клоном. Для этого обработка
formatting-элементов вынесена в `start_formatting(token)`, и табличный fallback
(«anything else») вызывает её же, а не свою упрощённую вставку.

Результат совпадает с html5lib: `<a>1</a><a>3</a><table>…`.

### 4. Adoption agency тоже уважает foster parenting

Шаг 16 алгоритма вставляет собранный узел в **common ancestor**, а им может
оказаться `<table>`. Теперь используется `insert_at(parent, node)`, который в
этом случае перевешивает узел перед таблицей.

### 5. `mode()` смотрит сквозь неструктурные элементы

Режим определяется ближайшим «структурным» элементом стека, а не верхним узлом:
`table` → Table, `tbody` → TableBody, `tr` → Row, `td` → Cell, `colgroup` →
ColumnGroup, `caption`/`template`/`html` → Body. Поэтому `<table><a>1<td>`
остаётся в правилах «in table», хотя сверху стека стоит `<a>`.

### 6. Режимы внутри `<template>`

Добавлен стек `template_modes_` (по записи на каждый открытый `<template>`,
изначально `Body`). В режиме «in template» стартовые теги `caption`, `colgroup`,
`tbody`, `tfoot`, `thead` переключают его на `Table`, `col` → `ColumnGroup`,
`tr` → `TableBody`, `td`/`th` → `Row`, после чего токен переразбирается. Поэтому
`<template><tr></tr><td></td></template>` даёт внутри `content` два `<tr>`
(второй — неявный), как в корпусе.

## Что работает

* `<table>A<td>B</td>C</table>` → `"AC"` перед таблицей;
* `<table><div>x</div></table>` → `<div>x</div><table></table>`;
* `<table><a>1<td>2</td>3</table>` → `<a>1</a><a>3</a><table>…`;
* `<table><a>1<p>2</a>3</p>` — adoption agency + foster parenting вместе;
* `<template><tr></tr><td></td></template>` — переключение режима внутри шаблона;
* все прошлые этапы не сломались: 348 юнит-тестов, `-Werror` чисто, ASan/UBSan
  без отчётов.

## Что не работает

Оставшиеся 251 падение tree-construction:

| кластер | случаев | причина |
|---|---|---|
| `template.dat` | 28 | `<frameset>` внутри `<template>`, `<template><td>` с последующими `<tbody>`/`<caption>` — нужен полный стек режимов и «in template» для всех групп токенов |
| `tests1/3/7/19` | ~59 | «in body» правила, которых у нас нет списком: `<h1>` в `<h1>`, `<menuitem>`, `</br>`, порядок `<select>` |
| `noscript` | 19 | нужен флаг scripting (сейчас всегда «scripting off») |
| `webkit01/02` | 31 | `<select>`-крайности, `noscript`, frameset-комбинации |
| `plain-text-unsafe.dat` | 15 | `plaintext`, CR/NUL-нормализация |
| остальное | ~100 | DOCTYPE-идентификаторы, комментарии `--!`, `<rb>` внутри `<p>` |

Вне конверта по-прежнему 412 случаев: fragment parsing и внешний контент
SVG/MathML. Также нет: рендеринга frameset'ов, `template.content` в JS,
абсолютного позиционирования, `border-collapse`, Date/RegExp/Map/Set/Promise,
классов и генераторов в JS.

## Как проверить

```bash
make -j                                        # сборка
make test                                      # 348 юнит-тестов
make verify                                    # сборка + тесты + selftest
./tools/fetch_conformance.sh                   # корпуса (~250 МБ, не в git)
make conformance                               # таблица + docs/conformance.md
./build/bonk_conformance --suite tree --max-failures 20
```

Строгий прогон:

```bash
make clean && make MODE=release WARN="-Wall -Wextra -Wpedantic -Wshadow -Werror" -j
make SAN=address,undefined MODE=debug -j test    # 348/348, ни одного ASan/UBSan отчёта
```

## Решение

[ADR 0013](adr/0013-foster-parenting-and-template-modes.md): foster parenting
реализован как свойство места вставки (одна функция `insert_node`/`insert_text`
для всего парсера), а не как флаг режима; режимы внутри шаблона — отдельный стек
`template_modes_`, потому что `<template>` — единственное место, где режим не
выводится из текущего узла.
