# Этап 24: массивы как объекты, `arguments`, `eval` и ранние ошибки

Этап 23 закончился честным счётом test262 и списком того, что он показал.
Первым в списке стоял `Array` (7.7 %): методы массива были **зашиты в сам объект
массива** — не существовало `Array.prototype`, поэтому `[] instanceof Array` было
`false`, `Array.prototype.isPrototypeOf([])` падало, а главное — нельзя было
написать `Array.prototype.slice.call(arguments)`, то есть весь классический
JavaScript «до ES6» был недоступен.

Заодно закрылись три соседние дыры, которые всплыли в том же прогоне: объект
`arguments` (1593 файла в `language` его упоминают), директный `eval` (282 файла
`eval-code`) и ранние ошибки вида `1 = 2` (245 файлов
`expressions/assignmenttargettype`), которые раньше падали в рантайме вместо
`SyntaxError` на разборе.

## Результат в цифрах

| набор | этап 23 | этап 24 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 453 | **462** | +9 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262 | 3 600 / 14 759 = 24.4 % | **4 511 / 14 759 = 30.6 %** | +911 тестов, +6.1 п.п. |

По каталогам:

| каталог | этап 23 | этап 24 |
|---|---|---|
| `Array` | 203 / 2 621 = 7.7 % | **680 / 2 621 = 25.9 %** |
| `language` | 2 308 / 6 530 = 35.3 % | **2 720 / 6 530 = 41.7 %** |
| `Object` | 607 / 3 225 = 18.8 % | 767 / 3 225 = 23.8 % |
| `String` | 121 / 1 071 = 11.3 % | 136 / 1 071 = 12.7 % |
| `Function` | 12 / 406 = 3.0 % | 16 / 406 = 3.9 % |

## Что сделано

### 1. `Array.prototype`: методы переехали из значения в прототип

Раньше `get_member` для массива отдавал замыкания, захватившие сам массив. Теперь
методы — обычные natives на одном общем `Array.prototype`
(`runtime.array_prototype`), а `this` приходит в них аргументом. Значение массива
несёт только `length` и индексный доступ.

Ключевое следствие: методы работают с **array-like**, а не только с массивами:
`length` + числовые ключи достаточно. Отсюда `Array.prototype.slice.call(arguments)`,
`Array.prototype.join.call(arguments, '-')`, `Array.prototype.map.call('abc', …)`
(строка приводится к array-like объекту, как `ToObject`).

Что появилось на прототипе: `push`, `pop`, `shift`, `unshift`, `slice`, `splice`,
`join`, `toString`, `indexOf`, `lastIndexOf`, `includes` (SameValueZero — `NaN`
находится), `concat`, `reverse`, `sort` (с компаратором и без), `forEach`, `map`,
`filter`, `find`, `findIndex`, `every`, `some`, `reduce`, `reduceRight`, `fill`.

Массивы стали настоящими экземплярами: `[] instanceof Array`, `Array.isArray`,
`Array.prototype.isPrototypeOf([])`, `[].constructor === Array`.

### 2. Объект `arguments`

Вместо «массива аргументов» теперь объект с `length` (неперечислимым), `callee`
и индексами. Главное — **связь с параметрами**: для индексов `0…n-1`, у которых
есть имя, индекс описан парой accessor-колбэков, читающих и пишущих слот параметра
в области вызова. Поэтому `arguments[0] = 1` меняет `p`, а `p = 1` видно как
`arguments[0]` — без копирования и без обхода области при каждом обращении.

Лишние фактические аргументы (без имени) просто лежат значениями; стрелочные
функции своего `arguments` не создают и видят внешний — как в стандарте.

### 3. Директный `eval`

`eval('...')`, вызванный как идентификатор, теперь разбирается и исполняется **в
области вызова**: `var` из eval попадает в текущую функцию, локальные переменные
видны. Результат — completion value скрипта, поэтому `eval('1 + 1')` равно 2.
Ошибка разбора превращается в `SyntaxError` (а не в тихую остановку скрипта).
Косвенный вызов (через переменную/`window.eval`) исполняется в глобальной области.

### 4. Ранние ошибки присваивания

`1 = 2`, `this = 1`, `f() = 1`, `(a, b) = 1`, `a++ = 2`, `'x' += 1` — теперь
`SyntaxError` на разборе (ES2015 12.14.1 AssignmentTargetType), а не
`ReferenceError` в рантайме. Разрешены: идентификаторы, обращения к свойствам,
а для `=` ещё и литералы массивов/объектов (синтаксис деструктуризации, которая
сама пока не реализована).

### 5. Числовые ключи в литералах объектов

`{0: 'a', 2.5: 'b'}` раньше вообще не разбирался («expected a property name»).
Это оказалось одной из самых дорогих мелочей этапа: +196 тестов test262.

### 6. Ошибки печатаются как в консоли

`Uncaught` теперь включает имя: `Uncaught TypeError: x is not a function`,
`Uncaught SyntaxError: …` — до этого имя класса терялось.

## Открытые пункты (следующий этап)

* `String.prototype` как объект: `String.prototype.trim.call(s)`,
  `String.prototype.slice` — обратная сторона проделанной работы для `Array`.
* `Function.prototype`/`Function.prototype.call` как значения-функции:
  `Function.prototype.call.call(f, null)`.
* `class`, деструктуризация, spread/rest, `for-of` — синтаксис.
* Strict-режим как таковой (директива `'use strict'`): ранние ошибки strict-only,
  запись в незаписываемые свойства с `TypeError`, `this === undefined` в вызовах
  без получателя.
* `RegExp`, `Date`, `Promise`, `Symbol` — целиком отсутствуют.

## Проверки

```console
$ make MODE=release -j8 && make test        # 462 теста, 0 падений
$ make conformance                          # -> docs/conformance.md
$ WARN="… -Werror" BUILD_DIR=build-strict make -j8   # 0 предупреждений
$ make SAN=address,undefined -j8 && ./build-address,undefined/bonk_tests
```

Решение и компромиссы — [`docs/adr/0026-array-prototype-arguments-and-eval.md`](adr/0026-array-prototype-arguments-and-eval.md).
