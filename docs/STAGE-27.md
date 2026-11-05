# Этап 27: арность встроенных функций и дескриптор `length` массива

Этап 26 научил движок отвечать за собственные свойства функций, но у встроенных
методов `length` оставался нулевым: арность C++-лямбды из `std::function` не
выводится, и `verifyProperty(Array.prototype.concat, 'length', {value: 1})`
продолжал падать. То же с `length` массива: он существовал «как бы» (его отдавал
`get_member`), но `hasOwnProperty('length')` был `false`, а
`getOwnPropertyDescriptor` — `undefined`.

## Результат в цифрах

| набор | этап 26 | этап 27 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 476 | **478** | +2 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262 | 6 850 / 14 759 = 46.4 % | **6 929 / 14 759 = 46.9 %** | +79 |

## Что сделано

### 1. Арность как часть контракта

Таблица «метод → арность» для `Array.prototype` (24 метода), `String.prototype`
(26), `Number.prototype`, `Boolean.prototype`, `Object.prototype`, статики
`Object`, `Array` (`from` = 1, `of` = 0, `isArray` = 1) и `String`
(`fromCharCode` = 1, `fromCodePoint` = 1). Значения проставляются в
`Function::parameter_count`, откуда их читают и `f.length`, и
`Object.getOwnPropertyDescriptor(f, 'length')`.

### 2. `length` массива — настоящее собственное свойство

`hasOwnProperty('length')` → `true`, `getOwnPropertyDescriptor([1,2,3], 'length')`
→ `{value: 3, writable: true, enumerable: false, configurable: false}`
(ES2015 9.4.2.1), `getOwnPropertyNames` включает `length`.

### 3. `Object.getPrototypeOf` знает про массивы и обёртки

`Object.getPrototypeOf([])` — `Array.prototype` (а не `Object.prototype`), у
`new String('x')` — `String.prototype`, и так же для `Number`/`Boolean`. Это
следствие того, что массивы и примитивы наследуются «неявно» (ADR 0026/0027):
прототип есть, ссылки в значении нет, поэтому `getPrototypeOf` отвечает по виду
значения.

## Открытые пункты (следующий этап)

* Strict-режим (`'use strict'`) — 351 файл «ожидалась ошибка разбора, получили
  ошибку рантайма» и все strict-only ранние ошибки.
* `Symbol` и `Symbol.iterator`/`for-of`: без них не работают `Array.prototype.keys/
  values/entries`, `Symbol.species` (а через него — `concat`/`slice`/`splice`).
* `Function.prototype` как объект.
* `class`, деструктуризация, spread/rest, `with` — 264 + 232 файла «statements are
  not supported».
* `RegExp`, `Date`, `Promise`, `Map`/`Set`, типизированные массивы.

## Проверки

```console
$ make MODE=release -j8 && make test        # 478 тестов, 0 падений
$ make conformance                          # -> docs/conformance.md
$ WARN="… -Werror" BUILD_DIR=build-strict make -j8   # 0 предупреждений
$ make SAN=address,undefined -j8 && ./build-address,undefined/bonk_tests
```
