# Этап 9: `<template>`, `<frameset>` и границы scope

Этап 8 закончился на 964/1 380 (69.9 %). Следующим по величине кластером падений
был `template.dat` — 108 случаев, и почти все они падали на одной фразе из
ожидаемого дерева: `content`. Это не косметика: `<template>` в стандарте хранит
детей **не в себе**, а в отдельном `DocumentFragment`. Пока такого узла нет,
нельзя ни разобрать `<template>` правильно, ни честно отрендерить страницу —
шаблон, у которого дети лежат прямо в элементе, начинает влиять на layout.

Второй по величине кластер — `<frameset>`: 108 падений в разных файлах
(`tests19`, `template`, `webkit01/02`). Движок всегда создавал `<body>`, поэтому
любой документ с frameset'ом расходился на первой же строке.

Оба пробела закрыты в этом этапе.

## Результат в цифрах

| набор | этап 8 | этап 9 | Δ |
|---|---|---|---|
| html5lib tree-construction | 964 / 1 380 = 69.9 % | **1 092 / 1 380 = 79.1 %** | +128 случаев, +9.2 п.п. |
| html5lib tokenizer | 6 281 / 6 820 = 92.1 % | 6 281 / 6 820 = 92.1 % | не трогали |
| test262 | 2 673 / 13 254 = 20.2 % | 2 673 / 13 254 = 20.2 % | не трогали |
| reftests | 7 / 7 | 7 / 7 | не трогали |
| собственные юнит-тесты | 333 | **339** | +6 |

Прирост по источникам:

| изменение | случаев |
|---|---|
| `template.dat` | 108 падений → 29 (то есть +79) |
| `<frameset>` (все файлы) | ~+49 |
| scope для «any other end tag» и блочных концов | +2 |
| `li`/`dd`/`dt` закрываются только внутри своего списка | +1 |

## Что сделано

### 1. `NodeType::Fragment` — у `<template>` появилось «content»

В DOM добавлен шестой тип узла — фрагмент (`Document::create_fragment()`).
Сериализуется он как свои дети (как и положено `DocumentFragment`), в отладочном
дереве печатается как `#fragment`, в раннере html5lib — строкой `content`.

Дерево-строитель делает фрагмент ребёнком `<template>` при его создании и
вставляет **внутрь фрагмента**, а не в элемент:

```cpp
[[nodiscard]] dom::NodeId insertion_parent() const {
  const dom::NodeId top = current();
  if (tag_of(top) == "template") {
    const dom::NodeId content = template_content(top);
    if (content.valid()) return content;
  }
  return top;
}
```

Это правило «appropriate place for inserting a node» из стандарта, и через него
теперь проходят все вставки: элементы, текст, клоны formatting-элементов и
фостер-родительство.

### 2. Режим «in template»

* `<template>` разрешён в `<head>` (до `<body>`) — иначе шаблон из `<head>`
  выбивал документ в `<body>`;
* при входе в шаблон в список активных formatting-элементов кладётся маркер,
  `</template>` снимает стек до шаблона и очищает список до этого маркера;
* содержимое шаблона разбирается по правилам `<body>`, но не создаёт `<body>`
  документу (`!body_created() && !has_open("template")`);
* `template_above_table()` отключает неявные `<tbody>/<tr>/<colgroup>`: `<td>`
  внутри `<template>` принадлежит шаблону, а не таблице.

### 3. `<frameset>`

Самый большой одиночный источник падений после template. Реализовано:

* флаг **frameset-ok** (как в стандарте): его снимают символы (не пробельные) и
  «прочие» стартовые теги; блочные теги его не снимают. Поэтому `<p><frameset>`
  даёт frameset (абзац пустой), а `<b>x<frameset>` — нет;
* `<frameset>` заменяет ещё пустой `<body>`: `unlink(body)`, frameset становится
  ребёнком `<html>`, стек — `[html, frameset]`, `body_` указывает на frameset
  (у frameset-документа нет body, но «корень контента» есть);
* внутри frameset: `<frame>` (void), вложенный `<frameset>`, `<noframes>`
  (raw text) — остальное игнорируется; текст, кроме пробельного, отбрасывается;
* после `</frameset>` документ считается frameset-документом: текст и посторонние
  теги отбрасываются, `<noframes>` и `<template>` продолжают работать;
* `<frame>` и `<frameset>` также вошли в список void-элементов DOM
  (`<frame>` больше не печатается как `<frame></frame>`).

### 4. Границы scope для закрывающих тегов

`</div>` не должен вылезать из `<template>`, `<td>` или `<table>`, `</span>` —
из открытого `<button>`. Сделано два правила:

* **блочные** концы (`div`, `ul`, `button`, `pre`, …) — свой список
  `kBlockEndTags`: генерация «implied end tags», затем pop; если элемент не в
  scope — тег игнорируется;
* **прочие** концы — «any other end tag»: идём вниз по стеку, на совпадении
  закрываем, на «специальном» элементе (`p`, `table`, `template`, …)
  останавливаемся и игнорируем тег.

Плюс `li`/`dd`/`dt` закрываются только внутри своего списка
(`tag_in_scope(tag, list_scope)`), поэтому `<ul><li>a<ul><li>b` наконец
**вкладывается**, а не расплющивается — это было видно на реальных страницах.

## Что работает

* `<template>` с содержимым в фрагменте; содержимое не попадает в layout
  (layout и CSS идут только по `NodeType::Element`);
* `</div>` не выходит из `<template>`: `<div><template></div>Hello` кладёт
  `"Hello"` в шаблон;
* frameset-документы разбираются: `<p><frameset><frame></frameset>` даёт
  `<html><head></head><frameset><frame></frameset></html>`;
* вложенные списки, `dd/dt`, диалоги, `<button>` и прочие блочные концы
  закрываются по scope, а не «до первого совпадения по имени»;
* демо: [`docs/demo/structure.html`](demo/structure.html) →
  [`docs/demo/structure.png`](demo/structure.png).

## Что не работает (и почему)

Оставшиеся 288 падений tree-construction:

| кластер | случаев | причина |
|---|---|---|
| таблица внутри `<template>` | 29 | нужен полноценный автомат insertion mode («in template» → «in table body» → «in row»); у нас один упрощённый режим |
| `tests19`, `tests3`, `tests7` | 57 | разнородные правила «in body»: `<h1>` внутри `<h1>`, `</p>` в неожиданных местах, порядок foster parenting |
| foster parenting рядом с таблицами | 74 (пересекается) | при перестановке узлов перед таблицей важен порядок вставки, у нас он приближённый |
| `noscript` | 19 | нужен флаг scripting; сейчас всегда «scripting off» |
| `webkit01/02` | 31 | `<select>`-крайности, `noscript`, `<frameset>` в сочетании с шаблонами |
| `plain-text-unsafe.dat` | 15 | `plaintext`, CR/NUL-нормализация в связке с деревом |
| остальное | ~120 | DOCTYPE-идентификаторы, `<menuitem>`, `<rb>` внутри `<p>`, комментарии `--!` |

Вне конверта по-прежнему 412 случаев: fragment parsing и внешний контент
SVG/MathML.

Также по-прежнему нет: рендеринга frameset'ов (парсим, но не рисуем),
`<template>`-клонов в JS (`content` не виден из скриптов), абсолютного
позиционирования, `border-collapse`, Date/RegExp/Map/Set/Promise/Symbol,
классов и генераторов в JS.

## Как проверить

```bash
make -j                                        # сборка
make test                                      # 339 юнит-тестов
make verify                                    # сборка + тесты + selftest
./tools/fetch_conformance.sh                   # корпуса (~250 МБ, не в git)
make conformance                               # таблица + docs/conformance.md
./build/bonk_conformance --suite tree --max-failures 20
./build/bonk view docs/demo/structure.html --screenshot /tmp/structure.png --width 820 --height 470
```

Строгий прогон:

```bash
make clean && make MODE=release WARN="-Wall -Wextra -Wpedantic -Wshadow -Werror" -j
make SAN=address,undefined MODE=debug -j test    # 339/339, ни одного ASan/UBSan отчёта
```

## Решение

[ADR 0011](adr/0011-template-contents-and-frameset.md): содержимое `<template>`
моделируется настоящим `DocumentFragment`, а не «детьми элемента с флажком»;
`<frameset>` — отдельный корневой контейнер вместо `<body>`. Оба выбора следуют
стандарту, а не удобству текущего кода, потому что иначе следующие этапы
(клон `content` в JS, рендеринг фреймов, инкрементальный парсинг) упрутся в
неверную модель.
