# Этап 33: `Symbol` и протокол итерации

Этап 32 закончился на 9 813 пройденных файлах test262 при 17 538 в конверте
(56.0 %). В разборе провалов две дырки были не «ещё один встроенный объект», а
два сквозных механизма языка:

* `Symbol` отсутствовал целиком (221 `ReferenceError`), а вместе с ним — все
  хорошо известные символы, символьные ключи свойств и `Symbol.for`;
* протокол итерации (`@@iterator`, объекты-итераторы, `for-of`, spread) не был
  реализован так, как его описывает стандарт: `for-of` перебирал массивы и
  строки «по массивам», `Array.prototype.values/keys/entries` не существовали,
  `[...x]` и `Array.from` не умели произвольные итерируемые объекты.

Из-за этого раннер вынужденно *исключал* из конверта все файлы с фичами
`Symbol`, `Symbol.iterator`, `Symbol.toStringTag`, `for-of` и
`computed-property-names` — больше тысячи файлов, которые движок не мог даже
попробовать. Этот этап закрывает оба механизма и **расширяет конверт**.

## Результат в цифрах

| набор | этап 32 | этап 33 | Δ |
|---|---|---|---|
| собственные юнит-тесты | 567 | **586** | +19 |
| пиксельные reftests | 13 / 13 | 13 / 13 | без изменений |
| html5lib tree-construction | 1 792 / 1 792 | 1 792 / 1 792 | без изменений |
| html5lib tokenizer | 6 792 / 6 820 | 6 792 / 6 820 | без изменений |
| test262: файлов в конверте | 17 538 | **18 391** | **+853** |
| test262: пройдено | 9 813 | **10 309** | **+496** |
| test262: провалено | 7 725 | 8 082 | +357 |
| test262: доля | 56.0 % | **56.1 %** | +0.1 п.п. |

Доля почти не выросла — и это ожидаемо: вместе с поддержкой в конверт попали
853 файла, которых раньше просто не запускали. 496 из них (58 %) проходят
сразу; остальные 357 — это уже настоящие пробелы (`Symbol.prototype`-тонкости,
TDZ, `arguments`, деструктуризация по итератору).

## Что сделано

### 1. `Symbol` как примитивный тип

`ValueType::Symbol` встал между `String` и `Object`; значение символа — это
уникальный `id` в `Value::number` плюс описание в `Value::text`. Отсюда
естественно получаются все требования стандарта:

* `typeof s === "symbol"`, `String(s) === "Symbol(desc)"`, `s.description`;
* `Symbol("a") === Symbol("a")` — `false` (`strict_equals` сравнивает `id`);
* `s === s` — `true`, `s == s` — тоже, но `s == "Symbol(x)"` — `false`;
* `JSON.stringify`, конкатенация и `Number(s)` ведут себя как для символа.

Глобальный `Symbol` реализует `Symbol(desc)` (с `TypeError` на `new`),
`Symbol.for`/`Symbol.keyFor` с реестром, `Symbol.prototype` с `toString`,
`valueOf`, `[@@toPrimitive]`, геттером `description`, `[@@toStringTag]` и
`constructor`, а также 12 хорошо известных символов — они **не записываемые**
(`{writable: false, enumerable: false, configurable: false}`), как того требует
20.4.2.

### 2. Символьные ключи свойств

Объект в движке хранит свойства в `std::map<std::string, Property>`, и заводить
вторую таблицу только ради символов не хотелось. Ключ символа кодируется как
однобайтовый префикс `0xFF` плюс `id`:

```cpp
constexpr char kSymbolKeyPrefix = static_cast<char>(0xFF);
std::string to_property_key(const Value& value);  // Symbol → "\xFF<id>", иначе to_string
```

Префикс `0xFF` в корректном UTF-8 встретиться не может, поэтому пространства
имён строк и символов не пересекаются, а `Object::properties` не изменился
вовсе. Достаточно было заменить `to_string(key)` на `to_property_key(key)` в
одиннадцати местах (литералы объектов, `o[k]`, деструктуризация, `in`,
`delete`, `defineProperty`…) и научить `own_property_keys` пропускать ключи с
префиксом: `Object.keys`, `for-in`, `JSON.stringify` и `Object.assign`
(строковая часть) символьных свойств не видят.

Обратная операция — `Object.getOwnPropertySymbols`: ключ `"\xFF<id>"`
разбирается обратно в символ через таблицу `RuntimeState::symbol_descriptions`
(`id → описание`). Порядок — по `id`, то есть по порядку создания символов;
это же делает результат детерминированным.

### 3. Протокол итерации

Вместо «перебрать массив» появились четыре функции уровня стандарта:

```cpp
bool  get_iterator(const Value& value, Value& iterator);   // GetIterator (7.4.2)
bool  iterator_next(const Value& iterator, Value& out);    // IteratorStep (7.4.5)
void  iterator_close(const Value& iterator);               // IteratorClose (7.4.9)
std::vector<Value> iterate_all(const Value& iterator);     // до `done` (с лимитом 1e6)
```

На них переведены `for-of`, spread (`[...x]`, `f(...x)`, `[...rest]`) и
`Array.from`. `for-of` шагает **лениво**: тело выходит из цикла через
`break`/`return`/`throw` — итератор закрывается (`return()` вызывается ровно
один раз), а ошибка хука не подменяет уже начатое завершение (`IteratorClose`
с throw-completion). `break` на третьем элементе делает три вызова `next()` и
ни одного лишнего — это проверяет юнит-тест.

`Array.prototype.values/keys/entries` и `[Symbol.iterator]` возвращают один и
тот же ленивый итератор, который сам итерируем (`it[Symbol.iterator]() === it`),
а `String.prototype[Symbol.iterator]` идёт **по код-пойнтам**: астральный
символ — один элемент, а не четыре байта. Тот же декодер UTF-8
(`string_code_points`) используется в spread, `Array.from` и деструктуризации.

Попутно: `for (… of null)` и `for (… of 5)` теперь `TypeError` (раньше это был
пустой цикл), `arguments` получил `@@iterator` (`%Array.prototype.values%`), а
`Object(primitive)` упаковывает значение в обёртку нужного вида — поэтому
`Object(sym).toString()` и `Object("ab").length` работают.

## Критерий выхода

Итерируемое значение — это то, у чего есть `@@iterator`, а не то, что «похоже
на массив»: пользовательский объект с `next()` перебирается `for-of`, отдаётся
в spread и в `Array.from`; символьные ключи невидимы для строкового
перечисления, но доступны через `Object.getOwnPropertySymbols`,
`Object.hasOwn` и `Object.assign`.

## Открыто

* `RegExp` (693 файла `built-ins/String`) и `Date` (152) — следующие крупные
  встроенные объекты;
* TDZ и блочные `let`/`const` (около 500 файлов `language/*`);
* `Symbol.species`/`Symbol.hasInstance`/`Symbol.unscopables` — символы есть,
  семантика (геттеры `@@species`, `instanceof` через `@@hasInstance`,
  `with`-невидимость) ещё нет;
* деструктуризация по итератору: `[a, b] = iterable` пока использует
  упрощённый разбор (`__bonk$arrayRest`), а не `IteratorClose`;
* `Symbol`-обёртки в `to_string`/`Number()`; полная валидация дескрипторов в
  `defineProperty`.
