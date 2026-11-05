# Этап 26: `assert` — это функция, собственные свойства функций, `Array.from`

Этот этап начался не в движке, а в раннере. Отчёт показывал 649 падений вида
`Uncaught TypeError: [object Object] is not a function` — и все они были в
`built-ins/Array/prototype/*`. Причина оказалась в преамбуле: `assert` из
`assert.js` — это **функция** с прикреплёнными методами
(`assert.sameValue`, `assert.throws`…), а наша заглушка объявляла
`var assert = {};`. Тесты вызывают `assert(cond, msg)` напрямую — значит, все
файлы, где встречался прямой вызов, падали на первой же проверке.

Ирония в том, что эта ошибка *занижала* счёт: настоящий движок эти тесты в
основном проходит. Ровно поэтому в этапе 23 был важен честный харнесс — но
честный харнесс тоже надо писать честно.

## Результат в цифрах

| набор | этап 25 | этап 26 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 472 | **476** | +4 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262 | 5 130 / 14 759 = 34.8 % | **6 850 / 14 759 = 46.4 %** | +1 720 тестов, +11.6 п.п. |

## Что сделано

### 1. `assert` — функция (главное)

`var assert = function (mustBeTrue, message) { … }` плюс прикреплённые методы.
Работает только потому, что функции в движке — объекты (этап 23): иначе
`assert.sameValue = …` на функции было бы невозможно.

### 2. Собственные свойства функций

У функции теперь есть настоящие `length`, `name` и `prototype` с правильными
дескрипторами: `length` — `writable: false, enumerable: false, configurable: true`
(значение — арность), `name` — то же самое, `prototype` — `writable: true,
enumerable: false, configurable: false`. Это отвечает `hasOwnProperty`,
`getOwnPropertyDescriptor`, `getOwnPropertyNames` и `verifyProperty` из
`propertyHelper.js` (до этого `verifyProperty(f, 'name', …)` сообщал «wrong
configurable»).

### 3. `instanceof` для внутренних ошибок

Ошибки, которые бросает сам интерпретатор (`null.x`, обращение к необъявленной
переменной, `not a function`), не создавались через конструктор, поэтому
`e instanceof TypeError` было `false`. Теперь `instanceof` для
`Kind::Error` отвечает по имени: `Error` — для всех, `TypeError` — для тех, у
кого `name === "TypeError"`.

### 4. `Array.from` и `Array.of`

`Array.from` (array-like и строка, с map-функцией и `thisArg`), `Array.of`.
Отсутствие `Array.from` давало самый частый «undefined is not a function» в
`built-ins/Array`.

### 5. Длина массива проверяется

`a.length = -1`, `a.length = 1.5`, `Object.defineProperty(a, 'length', {value: -5})`
теперь бросают `RangeError` (ES2015 9.4.2.4), а не молча обрезают массив.

## Открытые пункты (следующий этап)

* `Function.prototype` как объект: `Function.prototype.call.call(f, …)`,
  `bind` через прототип, `arguments.caller`.
* Арность (`length`) у встроенных методов: сейчас у natives `parameter_count` = 0,
  поэтому `verifyProperty(Array.prototype.concat, 'length', {value: 1})` падает
  (≈80 файлов `length.js`).
* `class`, деструктуризация, spread/rest, `for-of`/итераторы, `with`.
* Strict-режим, `Symbol`, `Symbol.species` (`Array.prototype.concat` через
  `Symbol`), `RegExp`, `Date`, `Promise`, `Map`/`Set`, типизированные массивы.
* `Object.getOwnPropertyDescriptor` для `arguments`-объекта и индексов строки.

## Проверки

```console
$ make MODE=release -j8 && make test        # 476 тестов, 0 падений
$ make conformance                          # -> docs/conformance.md
$ WARN="… -Werror" BUILD_DIR=build-strict make -j8   # 0 предупреждений
```
