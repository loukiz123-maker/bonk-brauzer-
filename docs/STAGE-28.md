# Этап 28: `class`, стандартный `ToString` чисел и ссылочная семантика

Этап 27 закончился на 46.9 % test262, и разбор полного корпуса показал три
крупных источника падений, не связанных с отсутствующими типами объектов:

* `Object.defineProperty(obj, 0.000001, {})` + `hasOwnProperty('0.000001')` —
  569 падений только на `defineProperty` и 306 на `defineProperties`;
* `'1e-7'` вместо `0.000001` в `String(x)` и в ключах свойств;
* `Array.prototype.map.call(true, …)`/`push.call(1)` — примитивы должны
  запаковываться в объект, а не бросать `TypeError`.

Плюс два «взрослых» пробела: `class` (2 284 файла `cls-*` в test262 и весь
современный код) и чтение неразрешённой ссылки, которое обязано быть
`ReferenceError`.

## Результат в цифрах

| набор | этап 27 | этап 28 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 478 | **498** | +20 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262 | 6 929 / 14 759 = 46.9 % | **7 290 / 14 759 = 49.4 %** | **+361** |

`language/` (не `built-ins/`) за этап: 2 720 → **3 383** из 6 530 (41.7 % → 51.8 %),
то есть синтаксис и семантика языка догоняют встроенные объекты.

## Что сделано

### 1. `ToString` числа по стандарту (ES2015 7.1.12.1)

Старая реализация печатала `%.*g`, поэтому `1e-6` превращалось в `1e-06`:
неверный формат экспоненты, нулевой паддинг и экспонента там, где стандарт
требует обычную запись. Теперь:

1. находится кратчайшая десятичная запись, которая парсится обратно в тот же
   `double` (`%.1g`…`%.17g`);
2. если `1e-6 ≤ |x| < 1e21` — запись разворачивается в обычную
   (`0.000001`, `100000000000000000000`);
3. иначе — экспоненциальная без паддинга (`1e-7`, `1e+21`, `5e-324`,
   `1.7976931348623157e+308`).

Так как `to_property_key` использует тот же `number_to_raw_string`, исправились
и ключи свойств: `Object.defineProperty(o, 0.000001, …)` создаёт свойство
`"0.000001"`, а `JSON.stringify({a: 1e-7})` даёт `{"a":1e-7}`.

### 2. `class`: конструктор, прототип, статика, `extends`/`super`

AST получил `ClassNode`/`ClassMethod` (ключ, функция, `static`/`get`/`set`),
парсер — `parse_class()` (в позиции объявления и в позиции выражения, имя
необязательно), интерпретатор — `Interpreter::make_class()`:

* метод `constructor` становится самой функцией-конструктором (наследуя
  `prototype_object`, `name` = имя класса);
* остальные методы кладутся в `prototype_object` как **неперечисляемые**,
  `static` — в `Function::statics`; `get`/`set` — в `receiver_getters`/
  `receiver_setters` с записью-маркером в `properties`, чтобы
  `Object.keys(C.prototype).length === 0`;
* `extends`: `C.prototype.__proto__ = S.prototype` и `C.__proto__ = S`
  (статическое наследование), поэтому `instanceof` и унаследованные статики
  работают по обычной цепочке;
* `super(...)` в конструкторе вызывает родительский конструктор с текущим
  `this`; `super.m(...)` ищет метод в `S.prototype` (с доступом к `this`
  вызывающего); без явного конструктора синтезируется
  `constructor (...args) { super(...args); }`;
* `Function::super_function` (weak) + переменная `super` в области видимости
  вызова — без отдельного объекта `[[HomeObject]]`.

`this` в геттерах/сеттерах прототипа тоже починен: `lookup_property` стал
принимать фактический получатель, иначе `get sum() { return this.x + this.y; }`
видел прототип вместо экземпляра (`NaN`).

### 3. `Function`, короткие методы и `constructor`

Добавлен глобальный `Function` (`new Function('a','b','return a+b')` парсит тело
при создании; `Function.length === 1`; `Function.prototype` с `call`,
`toString`), значения отвечают на `constructor` (`[].constructor === Array`,
`(5).constructor === Number`, `f.constructor === Function`). `__proto__` —
аксессор на `Object.prototype` (B.2.2.1), причём сеттер на неявном прототипе
теперь получает настоящий получатель.

### 4. Ссылочная семантика чтения

Чтение неразрешённого идентификатора — `ReferenceError: 'x' is not defined`
(ES2015 6.2.3.3). Исключения: `typeof x` → `"undefined"`, `delete x` → `true`,
присваивание `x = 1` по-прежнему создаёт глобальное свойство, а составное
`x *= 1` — бросает (оно читает ссылку). Это дало +213 в
`expressions/compound-assignment` и починило сообщения об ошибках:
`foo()` на необъявленной `foo` — теперь `ReferenceError`, а не
`undefined is not a function`.

### 5. Примитивы как array-like и дешёвые методы

* `Array.prototype.*.call(примитив)` больше не бросает: `ToObject(true)` не имеет
  `length`, то есть это пустой array-like (`push.call(true) === 0`);
* новые методы: `Array.prototype.at/copyWithin/flat/flatMap/findLast/
  findLastIndex/toLocaleString`, `Object.hasOwn`, `Object.getOwnPropertySymbols`
  (пустой массив — `Symbol` ещё нет), `String`/`Number` — без изменений;
* `Array.prototype.join` и `String.prototype.repeat` бросают `RangeError` при
  попытке построить строку больше 2^28 байт.

### 6. Большие `length` без аллокации

`a.length = 4294967295` раньше делал `elements.resize(4.29e9)` и падал с
`std::bad_alloc` (два файла test262). Появилось `Object::length_hint`: длина —
это `max(elements.size(), length_hint)`, хвост массива разрежённый, запись по
индексу растит и то и другое.

## Открытые пункты (следующий этап)

* `Symbol`/`Symbol.iterator`: без него не работают `keys/values/entries`,
  `Array.from` на итерируемых, `Symbol.species`, `for-of` по не-массивам.
* strict-режим (`'use strict'`) — 351 файл «ожидалась ошибка разбора».
* Деструктуризация, spread/rest, `switch`, `with`, `import()` — крупнейший
  синтаксический остаток (`expressions/assignment/dstr` — 86 файлов).
* `RegExp` (нужен почти всему `String.prototype.split/match/replace`), `Date`,
  `Promise`, `Map`/`Set`, типизированные массивы.
* `Object.defineProperty` с полной валидацией дескрипторов (569 падений —
  следующий по величине кластер после `Symbol`).

## Проверки

```console
$ make MODE=release -j8 && make test                     # 498 тестов, 0 падений
$ make conformance                                       # test262 7 290/14 759 (49.4 %)
$ WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror" \
      BUILD_DIR=build-strict make -j8                    # 0 предупреждений
$ make SAN=address,undefined -j8 && ./build-address,undefined/bonk_tests
                                                        # ASan/UBSan чисто, утечек нет
```
