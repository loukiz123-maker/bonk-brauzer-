# Этап 12: «in body» правила, table scope и судьба NUL

Этап 11 закончился на 1 129/1 380 (81.8 %) с честным списком: «нет полного набора
правил in body», «`noscript` со scripting», «`plaintext`». Здесь закрыта часть
этого списка — та, которая даёт системный эффект: **scope-проверки табличных
режимов**, правила заголовков и `<button>`, а также судьба символа NUL.

## Результат в цифрах

| набор | этап 11 | этап 12 | Δ |
|---|---|---|---|
| html5lib tree-construction | 1 129 / 1 380 = 81.8 % | **1 153 / 1 380 = 83.6 %** | +24 случая |
| html5lib tokenizer | 6 281 / 6 820 = 92.1 % | 6 281 / 6 820 = 92.1 % | без изменений |
| test262 | 2 673 / 13 254 = 20.2 % | 2 673 / 13 254 = 20.2 % | не трогали |
| reftests | 7 / 7 | 7 / 7 | не трогали |
| собственные юнит-тесты | 348 | **353** | +5 |

## Что сделано

### 1. Table scope: «таблица в scope» — это не «элемент в scope»

В табличных режимах мы проверяли `tag_in_scope(x)`, а эта функция останавливается
на `td`/`th`/`table` — то есть для `</table>` из ячейки она отвечала «таблицы в
scope нет». Стандарт для табличных режимов использует **table scope**, где
границы только `html`, `table`, `template`:

```cpp
[[nodiscard]] bool in_table_scope(std::string_view tag) const {
  for (std::size_t i = stack_.size(); i > 0; --i) {
    const std::string_view top = tag_of(stack_[i - 1]);
    if (top == tag) return true;
    if (top == "html" || top == "table" || top == "template") return false;
  }
  return false;
}
```

На этом вырос целый класс правильных отказов: «если в table scope нет
`tr`/`tbody`/`td` — токен игнорируется». Раньше мы в такой ситуации всё равно
закрывали стек и переразбирали токен, из-за чего `<table><td></td><tbody>` давал
`<tbody>` внутри `content` шаблона, а `<table><td>A</table>B` оставлял `B` в
ячейке. Теперь: `</table>` из ячейки закрывает ячейку, строку, секцию и таблицу,
а текст после этого попадает в `<body>`.

### 2. Заголовки закрываются по одному

`<h1>…<h2>` закрывает *только* заголовок, который является текущим узлом, а не
«любой заголовок где-то в стеке»: `<h1>one<div>two<h2>three` оставляет `<h2>`
внутри `<div>`, как в браузерах. Закрывающие теги `</h1>`…`</h6>` получили своё
правило: если в scope нет заголовка — игнорировать, иначе сгенерировать implied
end tags и снять стек до первого заголовка.

### 3. `<button>` закрывает `<button>`

Стартовый тег `<button>` при открытом `<button>` в scope генерирует implied end
tags и снимает стек до внешней кнопки; закрывающий `</button>` без кнопки в
scope игнорируется. `<button>a<b>b<button>c` даёт две кнопки, и `<b>`
реконструируется во второй — как в html5lib.

### 4. `</li>`, `</dd>`, `</dt>` — по list item scope

Закрывающие теги списков теперь проверяют scope (`tag_in_scope(tag, list_scope)`):
`</li>` вне списка игнорируется, а внутри — генерирует implied end tags (то есть
закрывает открытый `<p>` внутри пункта).

### 5. `<search>` закрывает `<p>`

Блочный элемент, которого не было в списке `kCloseParagraph`. Один из тех
случаев, где «правило» — это просто аккуратный список.

### 6. NUL: токенайзер пропускает, дерево решает

Раньше токенайзер в data state заменял U+0000 на U+FFFD. По стандарту это
неверно: в data state NUL **выдаётся как есть**, а правила приходят из режима
вставки — «in body» его игнорирует, текст внутри таблицы превращает в U+FFFD,
`<select>` игнорирует. Теперь так и сделано:

* токенайзер: `character_buffer_.push_back('\0')` без замены;
* дерево-строитель: `sanitize_text(text, replacement)` — в табличных режимах
  NUL → U+FFFD, в остальных выбрасывается.

Побочно пришлось поправить собственный фикстур
`tests/conformance/data/tokenizer/sample.test`: он ожидал `\uFFFD` для
`A\u0000` — ожидание приведено к стандарту (`A\u0000` на выходе токенайзера).

## Что работает

* `<table><td>A</table>B` → таблица, затем `"B"` в `<body>`;
* `<table><td>` + `<tbody>`/`<caption>`/`<colgroup>` вне scope — игнорируются;
* `<button>a<b>b<button>c` → две кнопки;
* `<h1>one<h2>two` → два заголовка; `<h1>one<div>two<h2>` → `<h2>` внутри `<div>`;
* `<p>a\0a</p>` → `"aa"`, NUL в тексте таблицы → U+FFFD;
* `<p>foo<search>bar<p>baz` → `<search>` как блочный элемент;
* строгая сборка `-Werror` чистая, ASan/UBSan 353/353 без отчётов (по дороге
  поймана реальная ошибка: `string_view` на временную строку в `sanitize_text`).

## Что не работает

Оставшиеся 227 падений tree-construction:

| кластер | случаев | причина |
|---|---|---|
| `template.dat` | ~25 | `<frameset>` внутри `<template>`, остальные группы токенов в «in template» |
| `tests1/3/16/19` | ~60 | точечные правила «in body» (`</br>` в особых местах, `<menuitem>`, порядок `<select>`, `<title>` в head) |
| `webkit01/02` | ~30 | `<select>`-крайности, `noscript` со scripting |
| `plain-text-unsafe.dat` | ~10 | `plaintext`, CR-нормализация в `<pre>` |
| остальное | ~100 | DOCTYPE-идентификаторы, внешний контент, редкие комбинации foster parenting |

Вне конверта по-прежнему 412 случаев: fragment parsing и внешний контент
SVG/MathML. Также нет: рендеринга frameset'ов, `template.content` в JS,
абсолютного позиционирования, `border-collapse`, Date/RegExp/Map/Set/Promise,
классов и генераторов в JS.

## Как проверить

```bash
make -j                                        # сборка
make test                                      # 353 юнит-теста
make verify                                    # сборка + тесты + selftest
./tools/fetch_conformance.sh                   # корпуса (~250 МБ, не в git)
make conformance                               # таблица + docs/conformance.md
./build/bonk_conformance --suite tree --max-failures 20
```

Строгий прогон:

```bash
make clean && make MODE=release WARN="-Wall -Wextra -Wpedantic -Wshadow -Werror" -j
make SAN=address,undefined MODE=debug -j test    # 353/353, ни одного ASan/UBSan отчёта
```

## Решение

[ADR 0014](adr/0014-table-scope-and-nul.md): табличные режимы проверяют именно
**table scope** (границы `html`/`table`/`template`), а NUL обрабатывается
дерево-строителем, а не токенайзером — оба решения следуют стандарту и убирают
целые классы расхождений, а не отдельные случаи.
