# Этап 17: DOCTYPE с идентификаторами, quirks по стандарту

Этап 16 закрыл дерево html5lib на 100 % *внутри конверта* — но 21 случай
дерева и 426 случаев токенайзера были выброшены из прогона, потому что наш
DOCTYPE-токен хранил только имя. Здесь это закрыто: у токена появились
`public_identifier` / `system_identifier` (и флаг `force_quirks`), токенайзер
получил полный state machine DOCTYPE, а парсер — определение quirks-режима по
стандарту. Идентификаторы дошли и до DOM, поэтому их печатает сериализатор.

## Результат в цифрах

| набор | этап 16 | этап 17 | Δ |
|---|---|---|---|
| html5lib tokenizer | 6 281 / 6 820 = 92.1 % (539 пропущено) | **6 707 / 6 820 = 98.3 %** (113 пропущено, 0 падений) | +426 |
| html5lib tree-construction | 1 535 / 1 535 = 100 % (21 случай вне конверта) | **1 556 / 1 556 = 100 %** (236 вне конверта) | +21 |
| test262 | 2 673 / 13 254 = 20.2 % | 2 673 / 13 254 = 20.2 % | не трогали |
| reftests | 7 / 7 | 7 / 7 | не трогали |
| собственные юнит-тесты | 392 | **395** | +3 |

Из прогона токенайзера ушли «skipped (doctype identifiers): 426», из дерева —
«skipped doctype-identifier cases: 21». Осталось вне конверта только 236
случаев внешнего контента (SVG/MathML) и 113 пропусков токенайзера
(56 — неизвестное начальное состояние, 3 — имена с префиксом пространства имён,
54 — задокументированные расхождения).

## Что сделано

### 1. Полный state machine DOCTYPE

Было: один общий стейт `Doctype`, который читал имя и «съедал суп из
идентификаторов» до `>`. Стало — 16 состояний по стандарту:
`before DOCTYPE name`, `DOCTYPE name`, `after DOCTYPE name`,
`after DOCTYPE public/system keyword`, `before DOCTYPE public/system identifier`,
`DOCTYPE public/system identifier (double/single quoted)`,
`after DOCTYPE public/system identifier`,
`between DOCTYPE public and system identifiers` и `bogus DOCTYPE`.

Правила, которые из этого следуют и видны в корпусе:

* кавычка сразу после `PUBLIC`/`SYSTEM` (без пробела) — не ошибка, а
  «missing-whitespace-…»: идентификатор начинается всё равно;
* `>` внутри идентификатора обрывает его (`abrupt-doctype-…`), а не закрывает
  кавычку: `<!DOCTYPE potato SYSTEM 'x>y'>` даёт system-identifier `x`;
* мусор после имени уводит в `bogus DOCTYPE`, который **сам** не включает
  force-quirks, но и не отменяет его; текст после `>` снова данные;
* EOF в состояниях, ожидающих кавычку, включает force-quirks; EOF в
  `bogus DOCTYPE` — нет. Именно на этом расходились 62 случая test3.test,
  пока флаг ставился в обоих случаях.

Флаг `force_quirks` — часть токена, поэтому тесты токенайзера сравнивают его
напрямую (поле `correctness` в `.test`).

### 2. Идентификаторы в DOM и в сериализаторе

`Document::create_doctype(name, public_id, system_id)` — перегрузка на случай,
когда идентификаторы есть (в том числе пустые: `PUBLIC ""` — это идентификатор,
а `<!DOCTYPE html>` — нет). Узел хранит два интернированных id, где `kNoNode`
означает «идентификатора не было».

* `Document::to_html()` печатает `<!DOCTYPE html "pub" "sys">`, если
  идентификаторы есть, и `<!DOCTYPE html>` иначе;
* раннер tree-корпуса печатает их так же, как html5lib (`<!DOCTYPE html
  "-//W3C//DTD HTML 4.01//EN" "http://…/strict.dtd">`), поэтому 21 случай
  `doctype01.dat`/`tests*.dat` пошёл в прогон и прошёл.

### 3. Quirks-режим по стандарту

`doctype_mode(token)` возвращает `Standards`, `LimitedQuirks` или `Quirks`:

* force-quirks, имя не `html`, три «точных» public-id
  (`-//W3O//DTD W3 HTML Strict 3.0//EN//`, `-/W3C/DTD HTML 4.0 Transitional/EN`,
  `HTML`), system-id IBM, 55 public-префиксов из стандарта → **quirks**;
* HTML 4.01 Frameset/Transitional **без** system-id → тоже полный quirks;
* XHTML 1.0 Frameset/Transitional и HTML 4.01 Frameset/Transitional **с**
  system-id → **limited quirks** (в парсере ведёт себя как standards: различие
  касается layout, а не разбора);
* всё остальное → standards.

Единственное правило парсера, которое зависит от режима, — `<table>` внутри
`<p>`; теперь оно реагирует на настоящие DOCTYPE, а не только на их отсутствие
(`<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN"><p><table>`
даёт таблицу внутри абзаца, а `…HTML 4.01//EN…` — соседнюю).

### 4. Мелочи, которые всплыли по дороге

* `Token` перестал быть «plain aggregate» для синтетических токенов: появился
  `plain_token()` (`<tbody>`/`<tr>`/`<colgroup>`, `</br>`, `</p>`), иначе
  добавленные поля давали `-Wmissing-field-initializers`;
* раннер токенайзера больше не печатает «skipped (doctype identifiers)» — счётчик
  удалён, потому что пропускать больше нечего.

## Проверки

* `make MODE=release WARN="… -Werror"` — 0 предупреждений;
* `make MODE=debug SAN=address,undefined` + `bonk_tests` + все четыре корпуса —
  чисто;
* `make verify` — OK; `make conformance` — пересобран `docs/conformance.md`;
* юнит-тестов 395 (+3): идентификаторы в токенайзере (включая `SYSTEM 'x>y'`,
  EOF и bogus-путь), идентификаторы в DOM/`to_html` и три режима quirks.

## Что осталось

| кластер | случаев | почему не сейчас |
|---|---|---|
| внешний контент SVG/MathML | 236 | нужны пространства имён в DOM, case-adjust, breakout-теги и интеграционные точки |
| неизвестное начальное состояние токенайзера | 56 | состояния, которые запускает только tree builder (CDATA и т.п.) |
| test262 | 10 581 | движок без большинства встроенных объектов (Array.prototype, String, Object…) |
