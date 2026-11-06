# ADR 0035: символы как отдельный тип и как отдельное пространство ключей

Дата: 2026-11-06. Этап: 33.

## Контекст

`Symbol` в движке отсутствовал: `typeof s` был невозможен, потому что не было
самого типа, а свойство с символьным ключом нельзя было ни создать, ни найти.
Требования стандарта, которые определяют реализацию:

1. Символ — **примитив**, а не объект: `typeof Symbol() === "symbol"`,
   `Object(sym)` — обёртка, `sym !== Object(sym)`.
2. Символы **уникальны**: `Symbol("a") === Symbol("a")` ложно, но
   `Symbol.for("a") === Symbol.for("a")` истинно (реестр).
3. Символы — **ключи свойств**, и это отдельное пространство имён: свойство с
   ключом `Symbol.iterator` не появляется в `Object.keys`/`for-in`/
   `JSON.stringify`, но видно в `Object.getOwnPropertySymbols` и
   `Object.getOwnPropertyDescriptor`.
4. Символ — не строка: `String(sym)` работает (`"Symbol(x)"`), а
   `sym + ""` и `Number(sym)` — `TypeError`.

## Решение

### 1. Значение символа = (`id`, описание)

```cpp
struct Value {
  ValueType type = ValueType::Undefined;   // Symbol — между String и Object
  double number = 0;    // для символа: уникальный id
  std::string text;     // для символа: описание
  ...
};
```

`id` монотонно растёт (`RuntimeState::next_symbol_id`), поэтому уникальность
символа — это равенство `id`, и `strict_equals` сводится к сравнению двух
чисел. Отдельного аллоцируемого объекта у символа нет: он примитив, копируется
по значению, живёт в `std::vector<Value>` как любое другое значение и не
участвует в подсчёте ссылок.

Отсутствие описания пришлось отличать от пустого описания: `Symbol()` даёт
`description === undefined`, `Symbol("")` — `""`. Поэтому кроме
`symbol_descriptions` (`id → строка`) есть множество `symbol_without_description`.

### 2. Ключ символа = префикс `0xFF` + `id`

Альтернативы:

| вариант | почему нет |
|---|---|
| Второй `std::map<SymbolId, Property>` в `Object` | удваивает каждый обход свойств: `own_property_keys`, `lookup_property`, `define_own_property`, `JSON`, `for-in`, все статические методы `Object` — каждый пришлось бы править и держать в голове порядок обхода |
| Хранить `Value` в ключе (`std::map<Value, Property>`) | ломает сравнение ключей, `std::map` требует `operator<`, а строка и символ должны быть разными ключами с разным порядком |
| Кодировать символ как обычную строку `"Symbol(x)"` | символы не уникальны, два `Symbol("x")` склеились бы в одно свойство |

Выбран третий путь: ключ — обычная строка, но начинающаяся с байта `0xFF`.
Корректный UTF-8 такого байта не содержит (там только `0x00–0x7F` и
`0xC2–0xF4`), значит коллизия со строковым ключом невозможна.

```cpp
constexpr char kSymbolKeyPrefix = static_cast<char>(0xFF);
std::string to_property_key(const Value& value);
bool is_symbol_key(const std::string& key);
```

Что это дало: `Object::properties` не изменился, порядок обхода строковых
свойств не изменился, а «невидимость» символов — это одна строка в
`own_property_keys` (`if (is_symbol_key(key)) return;`). Обратное
преобразование (`getOwnPropertySymbols`, `symbol_value_from_key`) идёт через
`symbol_descriptions`.

Двусмысленность «это ключ или строка?» решается на границе: всё, что приходит
из JS (`o[key]`, `o["k"]`, деструктуризация, `in`, `delete`, `defineProperty`,
`hasOwn`), проходит через `to_property_key`; всё, что уходит в JS как имя
свойства (`Object.keys`, `for-in`), фильтруется `is_symbol_key`.

### 3. Итерация — четыре функции, а не особый случай в цикле

Протокол итерации реализован ровно так, как он описан в стандарте
(`GetIterator`, `IteratorStep`, `IteratorClose`, `IteratorComplete`), и лежит
на уровне выше «массив/строка/array-like»:

```cpp
bool get_iterator(const Value& value, Value& iterator);   // 7.4.2
bool iterator_next(const Value& iterator, Value& out);    // 7.4.5
void iterator_close(const Value& iterator);               // 7.4.9
```

`for-of` больше не материализует список элементов: он тянет по одному и
закрывает итератор при досрочном выходе. Это принципиально: `break` на третьем
элементе не должен вызывать `next()` в четвёртый раз, а `return`/`throw` внутри
тела обязаны вызвать `return()` итератора (иначе `finally` в генераторе — когда
они появятся — не выполнится).

Строки перебираются по код-пойнтам: отдельный декодер UTF-8
(`string_code_points`) отдаёт по одной кодовой точке, а некорректные
последовательности заменяет на U+FFFD — один на максимальную невалидную часть,
как требует W3C/WHATWG.

### 4. Что осознанно не сделано

* `Symbol.species`, `Symbol.hasInstance`, `Symbol.unscopables`,
  `Symbol.isConcatSpreadable` — символы созданы и доступны, но семантика
  (геттеры видов, `instanceof` через `@@hasInstance`, невидимость имён в
  `with`, spread-поведение в `Array.prototype.concat`) не реализована.
  В конверте test262 эти фичи остаются исключёнными — иначе мы бы считали
  провалами то, чего не обещали.
* Деструктуризация `[a, b] = iterable` по-прежнему разворачивается парсером в
  `__bonk$arrayRest` (этап 21), а не в `GetIterator` + `IteratorClose`.
* `Symbol.prototype.description` — аксессор на прототипе, а не «свойство
  значения»: геттер получает `this` и достаёт символ из примитива или обёртки.

## Последствия

* Символьные ключи не ломают ни одного существующего пути обхода свойств —
  правок в `Object.keys`/`for-in`/`JSON` не потребовалось.
* `to_property_key` стал единственной точкой, где решается судьба ключа; новое
  место с `to_string(key)` — почти наверняка баг (так и нашлись
  `hasOwnProperty`, `propertyIsEnumerable`, `Object.hasOwn` и
  `getOwnPropertyDescriptor`).
* Конверт test262 расширен: сняты `Symbol`, `Symbol.iterator`,
  `Symbol.toStringTag`, `for-of`, `computed-property-names`; в
  `supported_builtins` добавлены `Symbol` и `String.prototype`.
* `Object(primitive)` теперь упаковывает значение в обёртку нужного вида
  (`wrapper_value` + прототип), поэтому `Object("ab").length`,
  `Object(5).valueOf()` и `Object(sym).toString()` согласованы с
  `new String(…)`.
