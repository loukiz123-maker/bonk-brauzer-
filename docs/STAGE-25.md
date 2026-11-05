# Этап 25: прототипы примитивов и примитивы-обёртки

Этап 24 сделал `Array.prototype` — и сразу стало видно, что ровно та же болезнь у
строк: методы `String` были замыканиями на конкретном значении строки, а самого
`String.prototype` не существовало. Отчёт показывал `built-ins/String/prototype` —
427 падений, крупнейший кластер среди всех встроенных объектов после `Object`.

Плюс две мелочи с тем же корнем (примитивы и их объекты): не было обёрток
`new String('x')`/`new Number(1)`/`new Boolean(true)` и не разбирались составные
присваивания `<<=`, `>>=`, `>>>=`, `&=`, `|=`, `^=`, `**=` — из-за этого 344 теста
`expressions/compound-assignment` падали на разборе.

## Результат в цифрах

| набор | этап 24 | этап 25 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 462 | **472** | +10 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262 | 4 571 / 14 759 = 31.0 % | **5 130 / 14 759 = 34.8 %** | +559 тестов, +3.8 п.п. |

По каталогам:

| каталог | этап 24 | этап 25 |
|---|---|---|
| `String` | 136 / 1 071 = 12.7 % | **266 / 1 071 = 24.8 %** |
| `Number` | 48 / 280 = 17.1 % | **74 / 280 = 26.4 %** |
| `Boolean` | 14 / 46 = 30.4 % | **17 / 46 = 37.0 %** |
| `Array` | 680 / 2 621 = 25.9 % | 698 / 2 621 = 26.6 % |
| `Object` | 767 / 3 225 = 23.8 % | 773 / 3 225 = 24.0 % |

## Что сделано

### 1. `String.prototype`

Методы переехали из значения строки в общий `String.prototype`; в самом значении
остались только `length` и индексный доступ. `this` разворачивается либо в
примитив, либо в обёртку (`string_this`), поэтому одинаково работают
`"abc".trim()`, `String.prototype.trim.call("  abc  ")` и `new String("abc").trim()`.

На прототипе: `toString`, `valueOf`, `toUpperCase`, `toLowerCase`, `trim`,
`trimStart`, `trimEnd`, `charAt`, `charCodeAt`, `codePointAt`, `at`, `indexOf`,
`lastIndexOf`, `includes`, `startsWith`, `endsWith`, `concat`, `slice`,
`substring`, `substr`, `padStart`, `padEnd`, `repeat`, `split`, `replace`
(строковый образец или функция-заменитель), `localeCompare`.

Методы, которых раньше не было вообще: `substring`, `substr`, `trimStart/End`,
`charCodeAt`, `codePointAt`, `at`, `padStart/padEnd`, `repeat`, `concat`,
`localeCompare`.

### 2. `Number.prototype` и `Boolean.prototype`

`toString(radix)` (2…36, отрицательные и дробные), `toFixed`, `toPrecision`,
`toExponential`, `valueOf` — на `Number.prototype`, а не на значении. У
`Boolean.prototype` — `toString`/`valueOf`.

Статика `Number`: `MAX_VALUE`, `MIN_VALUE`, `EPSILON`, `MAX_SAFE_INTEGER`,
`MIN_SAFE_INTEGER`, `POSITIVE_INFINITY`, `NEGATIVE_INFINITY`, `NaN`, `isNaN`,
`isFinite`, `isInteger`, `parseInt`, `parseFloat`. Статика `String`:
`fromCharCode`, `fromCodePoint` (обе кодируют в UTF-8, как и весь движок).

### 3. Обёртки примитивов

`new String('x')`, `new Number(1)`, `new Boolean(true)` создают объект с
запакованным примитивом (`Object::is_wrapper` + `wrapper_value`):

* `typeof` — `"object"`, `Object.prototype.toString` — `[object String]` и т. д.;
* `to_number`/`to_string` разворачивают обёртку, поэтому `new Number(41) + 1` = 42
  и `"x = " + new String("y")` даёт `x = y`;
* `.valueOf()`/`.toString()` возвращают примитив, `.length` у строковой обёртки
  работает, `"abc".constructor === String`.

### 4. Полный набор составных присваиваний

Лексер и парсер знают `<<=`, `>>=`, `>>>=`, `&=`, `|=`, `^=`, `**=`. В
интерпретаторе появилась одна общая функция `apply_arithmetic`, которой
пользуются и бинарные выражения, и составные присваивания (до этого `x <<= 1`
просто не разбиралось, а `x >>> y` не работало вовсе). Заодно появились честные
`ToInt32`/`ToUint32` — от них зависят сдвиги: `-1 >>> 0` = 4294967295,
`1 << 31` = -2147483648, `NaN | 0` = 0.

## Открытые пункты (следующий этап)

* `Function.prototype` как объект (`Function.prototype.call.call(f, …)`,
  `f.bind` через прототип).
* `class`, деструктуризация, spread/rest, `for-of`, `with` — синтаксис.
* Strict-режим: ранние ошибки strict-only, `this === undefined` в вызовах без
  получателя, `TypeError` при записи в незаписываемое свойство.
* Строки считаются в UTF-8-байтах: `"😀".length` = 4, а не 2. Нужен переход на
  UTF-16-единицы, чтобы совпадать со стандартом (и это затронет `slice`,
  `charCodeAt`, индексацию).
* `RegExp`, `Date`, `Promise`, `Symbol`, `Map`/`Set` — отсутствуют.

## Проверки

```console
$ make MODE=release -j8 && make test        # 472 теста, 0 падений
$ make conformance                          # -> docs/conformance.md
$ WARN="… -Werror" BUILD_DIR=build-strict make -j8   # 0 предупреждений
$ make SAN=address,undefined -j8 && ./build-address,undefined/bonk_tests
```

Решение и компромиссы — [`docs/adr/0027-primitive-prototypes-and-wrappers.md`](adr/0027-primitive-prototypes-and-wrappers.md).
