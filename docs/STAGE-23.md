# Этап 23: честный test262 и свойства объектов

Два дела в одном этапе, потому что первое измерило второе.

Сначала выяснилось, что **20.2 % test262 были неправдой**: преамбула раннера
подсовывала заглушки вместо harness-файлов test262. `assert.throws` был написан
как «вызвать функцию и бросить свою ошибку» (то есть проходил всегда, даже когда
исключения не было), `verifyProperty` ничего не проверял, `assert.sameValue`
сравнивал через `!==` (NaN не равен сам себе, `+0` и `-0` не различались), а
**любой** `includes:` приводил к пропуску файла — так молча выпали 1651 тест.

После починки харнесса счёт стал честным и **упал** до 18.2 %. Дальше этот же
честный счёт показал, где на самом деле дыра: `Object` проходил 0.3 %
(8 из 2366). Причина одна — в движке не было дескрипторов свойств.

## Результат в цифрах

| набор | этап 22 (старый харнесс) | этап 23 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 442 | **453** | +11 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262 (честный харнесс) | 2 692 / 14 759 = 18.2 % | **3 600 / 14 759 = 24.4 %** | +908 тестов, +6.2 п.п. |
| test262 (старая, завышенная цифра) | 2 673 / 13 254 = 20.2 % | — | не сравниваем |

По каталогам `built-ins` (тот же честный харнесс, изолированные корпуса):

| каталог | этап 22 | этап 23 |
|---|---|---|
| `Object` | 8 / 2 366 = **0.3 %** | 607 / 3 225 = **18.8 %** |
| `Array` | 145 / 2 427 = 6.0 % | 203 / 2 621 = 7.7 % |
| `Error` | 0 / 36 = 0.0 % | 2 / 53 = 3.8 % |
| `Function` | 10 / 381 = 2.6 % | 12 / 406 = 3.0 % |

Знаменатели выросли, потому что поддерживаемых `includes:` стало больше — файлы,
которые раньше пропускались целиком, теперь действительно исполняются.

## Что сделано

### 1. Харнесс test262 больше не врёт

`kPreamble` теперь — честная реализация того, что тесты ожидают от
`sta.js`/`assert.js`:

* `Test262Error` как отдельный конструктор и `$DONOTEVALUATE()`;
* `$sameValue` по стандарту (NaN равен NaN, `+0` и `-0` различаются) и на нём
  `assert.sameValue` / `assert.notSameValue`;
* `assert.throws` проверяет, что исключение **есть**, и умеет конструктор или
  объект-образец (`{name: 'TypeError'}`), `assert.compareArray`,
  `assert.defineProperty`, `assert.objKeys`, `assert.fail`;
* `verifyProperty(obj, name, desc)` — полноценная проверка дескриптора:
  наличие, `value`/`writable`/`enumerable`/`configurable`, `get`/`set`;
* `comparePropertyDescriptor`, `isConstructor`, `$262`.

Вместо правила «есть `includes:` → пропустить» появился явный список
`supported_includes()`: `assert.js`, `sta.js`, `propertyHelper.js`,
`compareArray.js`, `isConstructor.js`. Всё остальное (`detachArrayBuffer.js`,
`temporalHelpers.js`, …) по-прежнему пропускается, но уже **явно** и с
отдельным счётчиком в отчёте (146 файлов).

Побочный эффект: тесты, которые раньше «проходили» на заглушках, теперь честно
падают. Процент поэтому сначала упал — это и есть цель этапа.

### 2. Дескрипторы свойств

`Property` получил `configurable`, `Object` — `extensible` и
`prototype_is_null`. Появилось `[[DefineOwnProperty]]` с проверками
неконфигурируемых свойств (ES2015 9.1.6.3 — та часть, которую тесты реально
проверяют):

* нельзя сделать конфигурируемым неконфигурируемое;
* нельзя сменить `enumerable` у неконфигурируемого;
* нельзя превратить data-свойство в accessor и обратно;
* нельзя переписать значение у неконфигурируемого **незаписываемого** свойства;
* запись в незаписываемое свойство и добавление свойства в нерасширяемый объект
  в sloppy-коде молча игнорируются (strict-режим — открытый пункт);
* `delete` уважает `configurable` и возвращает `false`.

### 3. `Object` и `Object.prototype`

Статика: `keys`, `values`, `entries`, `getOwnPropertyNames`,
`getOwnPropertyDescriptor`, `defineProperty`, `defineProperties`, `create`,
`getPrototypeOf`, `setPrototypeOf`, `freeze`, `seal`, `preventExtensions`,
`isFrozen`, `isSealed`, `isExtensible`, `is`, `assign`. У `Array` появился
`Array.isArray` и `Array(n)` (одиночное число — длина).

`Object.prototype` — теперь **один** объект, от которого наследуются все
объекты: `hasOwnProperty`, `propertyIsEnumerable`, `isPrototypeOf`, `toString`
(`[object Array]`/`[object Null]`/`[object Function]`/…), `valueOf`,
`toLocaleString`. `Object.create(null)` действительно обрывает цепочку.

Ключи собственных свойств выдаются в детерминированном порядке: сначала
индексы по возрастанию, затем строки по алфавиту. `std::unordered_map` порядок
вставки не хранит, поэтому «как в браузере» порядок не гарантируется — это
зафиксировано в ADR 0025.

### 4. Функции — тоже объекты

Раньше `f.custom = 1` бросало «cannot set property … of function», а
`Object.defineProperty(f, …)` — «на non-object». Теперь у `Function` есть
собственный мешок свойств (`Function::statics`), в который пишут и `set`, и
статика `Object`, и `hasOwnProperty`; `f.prototype.constructor === f`
выполняется без цикла `shared_ptr` (ссылка на функцию в её прототипе —
`weak_ptr`, иначе каждый прототип держал бы функцию вечно).

### 5. Accessor-свойства с получателем

`{get x() {…}, set x(v) {…}}` в литералах объектов и `get`/`set` в
`Object.defineProperty`. Внутри аксессора доступен `this` — для этого в
`Object` появились `receiver_getters`/`receiver_setters` (bindings слоя DOM
продолжают пользоваться прежними `getters`/`setters`, которым `this` не нужен).

### 6. Обход свойств

`for-in` идёт по собственным перечислимым ключам (с индексами массива),
неперечислимые свойства (`enumerable: false`) больше не видны.

## Открытые пункты (следующий этап)

* `Array.prototype` как объект-неймспейс: `Array.prototype.push.call(...)`,
  `Array.prototype.isPrototypeOf([])` — отсюда большинство оставшихся падений
  в `built-ins/Array`; массив сейчас несёт методы сам, без прототипа.
* `arguments` (1593 файла в языке упоминают его): нужен объект с `callee` и
  связью с параметрами.
* `class`, деструктуризация, spread/rest, `for-of` — синтаксис.
* Strict-режим: `'use strict'` пока не влияет на семантику записи.
* `Object.defineProperty` на `Object.create(null)`-объектах без прототипа:
  `toString`/`hasOwnProperty` там отсутствуют, как и должно быть, но многие
  тесты ожидают `TypeError` там, где мы молча пишем.

## Проверки

```console
$ make MODE=release -j8 && make test        # 453 теста, 0 падений
$ make conformance                          # -> docs/conformance.md
$ WARN="… -Werror" BUILD_DIR=build-strict make -j8   # 0 предупреждений
$ make SAN=address,undefined -j8 && ./build-address,undefined/bonk_tests
```

Решение и компромиссы — [`docs/adr/0025-property-descriptors-and-object-statics.md`](adr/0025-property-descriptors-and-object-statics.md).
