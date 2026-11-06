// Stage 33: `Symbol` primitives, symbol-keyed properties and the iteration
// protocol (`@@iterator` / iterator objects, `for-of`, spread, `Array.from`).
#include <string>

#include "test_framework.hpp"

#include "bonk/js/js.hpp"

using namespace bonk;
using namespace bonk::js;

namespace {

Value eval_global(const std::string& source, const std::string& name) {
  Context context;
  const ExecutionResult result = context.evaluate(source);
  CHECK(result.ok);
  if (!result.ok) return Value::undefined();
  return context.runtime().global(name);
}

double num(const std::string& source, const std::string& name = "v") { return to_number(eval_global(source, name)); }
std::string str(const std::string& source, const std::string& name = "v") {
  return to_string(eval_global(source, name));
}
bool fails_with(const std::string& source, const std::string& needle) {
  Context context;
  const ExecutionResult result = context.evaluate(source);
  return !result.ok && result.message.find(needle) != std::string::npos;
}

}  // namespace

// ------------------------------------------------------------- primitives ----

BONK_TEST(js_symbol, typeof_and_conversion) {
  CHECK_EQ(str("var s = Symbol(); var v = typeof s;"), std::string("symbol"));
  CHECK_EQ(str("var s = Symbol('desc'); var v = String(s);"), std::string("Symbol(desc)"));
  CHECK_EQ(str("var s = Symbol('desc'); var v = s.toString();"), std::string("Symbol(desc)"));
  CHECK_EQ(str("var v = String(Symbol());"), std::string("Symbol()"));
  CHECK_EQ(str("var s = Symbol('x'); var v = s.description;"), std::string("x"));
  CHECK_EQ(str("var v = Symbol('x').valueOf() === Symbol('x');"), std::string("false"));
  CHECK_EQ(str("var s = Symbol('x'); var v = s.valueOf() === s;"), std::string("true"));
  CHECK_EQ(str("var s = Symbol('x'); var v = s.constructor === Symbol;"), std::string("true"));
}

BONK_TEST(js_symbol, symbols_are_unique_but_objects_are_not) {
  CHECK_EQ(str("var v = Symbol('a') === Symbol('a');"), std::string("false"));
  CHECK_EQ(str("var s = Symbol('a'); var v = s === s;"), std::string("true"));
  CHECK_EQ(str("var v = Symbol() == Symbol();"), std::string("false"));
}

BONK_TEST(js_symbol, registry_round_trips) {
  CHECK_EQ(str("var v = Symbol.for('k') === Symbol.for('k');"), std::string("true"));
  CHECK_EQ(str("Symbol.for('k2'); var v = Symbol.keyFor(Symbol.for('k2'));"), std::string("k2"));
  CHECK_EQ(str("var v = String(Symbol.keyFor(Symbol('free')));"), std::string("undefined"));
  // A registry symbol keeps the description given on first registration.
  CHECK_EQ(str("Symbol.for('d'); var v = Symbol.for('d').description;"), std::string("d"));
}

BONK_TEST(js_symbol, well_known_symbols_exist_and_are_unique) {
  CHECK_EQ(str("var v = typeof Symbol.iterator;"), std::string("symbol"));
  CHECK_EQ(str("var v = Symbol.iterator === Symbol.for('Symbol.iterator');"), std::string("false"));
  CHECK_EQ(str("var v = Symbol.iterator === Symbol.iterator;"), std::string("true"));
  CHECK_EQ(str("var v = [Symbol.asyncIterator, Symbol.hasInstance, Symbol.isConcatSpreadable,"
               "Symbol.species, Symbol.toPrimitive, Symbol.toStringTag, Symbol.unscopables,"
               "Symbol.match, Symbol.replace, Symbol.search, Symbol.split].every(function (s) {"
               "return typeof s === 'symbol'; });"),
           std::string("true"));
  CHECK_EQ(str("var v = String(Symbol.iterator);"), std::string("Symbol(Symbol.iterator)"));
}

// -------------------------------------------------------- symbol-keyed props ----

BONK_TEST(js_symbol, symbol_keys_are_invisible_to_string_key_enumeration) {
  const std::string setup = "var s = Symbol('k'); var o = {}; o[s] = 1; o.plain = 2; ";
  CHECK_EQ(num(setup + "var v = o[s];"), 1.0);
  CHECK_EQ(num(setup + "var v = Object.keys(o).length;"), 1.0);
  CHECK_EQ(str(setup + "var v = Object.getOwnPropertyNames(o).join(',');"), std::string("plain"));
  CHECK_EQ(str(setup + "var v = JSON.stringify(o);"), std::string("{\"plain\":2}"));
  CHECK_EQ(num(setup + "var n = 0; for (var k in o) n++; var v = n;"), 1.0);
  CHECK_EQ(str(setup + "var v = s in o;"), std::string("true"));
  CHECK_EQ(str(setup + "var v = 'Symbol(k)' in o;"), std::string("false"));
}

BONK_TEST(js_symbol, get_own_property_symbols) {
  CHECK_EQ(num("var s = Symbol('k'); var o = {}; o[s] = 1; var v = Object.getOwnPropertySymbols(o).length;"),
           1.0);
  CHECK_EQ(str("var s = Symbol('k'); var o = {}; o[s] = 1; var v = Object.getOwnPropertySymbols(o)[0] === s;"),
           std::string("true"));
  CHECK_EQ(str("var s = Symbol('k'); var o = {}; o[s] = 1;"
               "var v = String(Object.getOwnPropertySymbols(o)[0]);"),
           std::string("Symbol(k)"));
  CHECK_EQ(num("var v = Object.getOwnPropertySymbols({a: 1}).length;"), 0.0);
  CHECK_EQ(num("var v = Object.getOwnPropertySymbols([]).length;"), 0.0);
  // Ordered by symbol creation order, not by insertion into the object.
  CHECK_EQ(str("var a = Symbol('a'); var b = Symbol('b'); var o = {}; o[b] = 1; o[a] = 2;"
               "var v = Object.getOwnPropertySymbols(o).map(String).join(',');"),
           std::string("Symbol(a),Symbol(b)"));
}

BONK_TEST(js_symbol, descriptor_apis_accept_symbol_keys) {
  CHECK_EQ(str("var s = Symbol('k'); var o = {}; o[s] = 5;"
               "var v = Object.getOwnPropertyDescriptor(o, s).value;"),
           std::string("5"));
  CHECK_EQ(str("var s = Symbol('k'); var o = {};"
               "Object.defineProperty(o, s, {value: 7, writable: true, enumerable: true, configurable: true});"
               "var v = o[s];"),
           std::string("7"));
  CHECK_EQ(str("var s = Symbol('k'); var d = {}; d[s] = {value: 3};"
               "var o = Object.create(null, d); var v = o[s];"),
           std::string("3"));
}

BONK_TEST(js_symbol, has_own_and_assign_see_symbol_keys) {
  const std::string setup = "var s = Symbol('k'); var o = {}; o[s] = 1; ";
  CHECK_EQ(str(setup + "var v = Object.hasOwn(o, s);"), std::string("true"));
  CHECK_EQ(str(setup + "var v = o.hasOwnProperty(s);"), std::string("true"));
  CHECK_EQ(str(setup + "var v = o.propertyIsEnumerable(s);"), std::string("true"));
  CHECK_EQ(str(setup + "var c = Object.assign({}, o); var v = c[s];"), std::string("1"));
  CHECK_EQ(num(setup + "var v = Object.getOwnPropertySymbols(Object.assign({}, o)).length;"), 1.0);
}

BONK_TEST(js_symbol, delete_removes_symbol_keys) {
  CHECK_EQ(str("var s = Symbol('k'); var o = {}; o[s] = 1; delete o[s];"
               "var v = String(o[s]) + '/' + Object.getOwnPropertySymbols(o).length;"),
           std::string("undefined/0"));
}

// ------------------------------------------------------------ iterators ----

BONK_TEST(js_iteration, for_of_walks_arrays_and_strings) {
  CHECK_EQ(str("var out = []; for (var x of [1, 2, 3]) out.push(x); var v = out.join(',');"),
           std::string("1,2,3"));
  CHECK_EQ(str("var out = []; for (const c of 'abc') out.push(c); var v = out.join(',');"),
           std::string("a,b,c"));
  CHECK_EQ(num("var n = 0; for (var x of []) n++; var v = n;"), 0.0);
}

BONK_TEST(js_iteration, for_of_stops_at_done_and_pulls_lazily) {
  CHECK_EQ(num("var pulls = 0; var it = {};"
               "it[Symbol.iterator] = function () { return { next: function () { pulls++;"
               "return {value: pulls, done: pulls > 100}; } }; };"
               "for (var x of it) { if (x === 3) break; } var v = pulls;"),
           3.0);
  CHECK_EQ(str("var it = { i: 0 }; it[Symbol.iterator] = function () { var self = this;"
               "return { next: function () { self.i++; return {value: self.i, done: self.i > 2}; } }; };"
               "var out = []; for (var x of it) out.push(x); var v = out.join(',');"),
           std::string("1,2"));
}

BONK_TEST(js_iteration, for_of_rejects_non_iterables) {
  CHECK(fails_with("for (var x of 5) {}", "not iterable"));
  CHECK(fails_with("for (var x of null) {}", "not iterable"));
  CHECK(fails_with("for (var x of {}) {}", "not iterable"));
}

BONK_TEST(js_iteration, array_iterator_methods) {
  CHECK_EQ(str("var v = [10, 20].values().next().value;"), std::string("10"));
  CHECK_EQ(str("var it = [10].values(); it.next(); var v = it.next().done;"), std::string("true"));
  CHECK_EQ(str("var v = [7, 8].keys().next().value;"), std::string("0"));
  CHECK_EQ(str("var v = ['a'].entries().next().value.join('=');"), std::string("0=a"));
  CHECK_EQ(str("var v = Array.from([7, 8].keys()).join(',');"), std::string("0,1"));
  CHECK_EQ(str("var v = Array.prototype.values === Array.prototype[Symbol.iterator];"), std::string("true"));
  CHECK_EQ(str("var it = [1].values(); var v = it[Symbol.iterator]() === it;"), std::string("true"));
  CHECK_EQ(str("var v = String.prototype[Symbol.iterator] === undefined ? 'no' : 'yes';"), std::string("yes"));
}

BONK_TEST(js_iteration, spread_and_array_from_use_iterator_protocol) {
  CHECK_EQ(str("var v = [1, ...[2, 3], ...'ab'].join(',');"), std::string("1,2,3,a,b"));
  CHECK_EQ(num("var v = Math.max(...[3, 9, 4]);"), 9.0);
  CHECK_EQ(num("var it = {}; var v = Symbol.iterator in it ? 1 : 0;"), 0.0);
  CHECK_EQ(str("var v = Array.from('ab').join('-');"), std::string("a-b"));
  CHECK_EQ(str("var v = Array.from({length: 2, 0: 'x', 1: 'y'}).join('-');"), std::string("x-y"));
}

BONK_TEST(js_iteration, destructuring_rest_uses_the_iterator) {
  CHECK_EQ(num("var [a, ...rest] = [1, 2, 3]; var v = a + rest.length;"), 3.0);
  CHECK_EQ(str("var [...all] = 'xyz'; var v = all.join('');"), std::string("xyz"));
}

BONK_TEST(js_symbol, prototype_methods) {
  CHECK_EQ(str("var s = Symbol('d'); var v = s.toString();"), std::string("Symbol(d)"));
  CHECK_EQ(str("var s = Symbol('d'); var v = Object(s).valueOf() === s;"), std::string("true"));
  CHECK_EQ(str("var s = Symbol('d'); var v = String(Object(s));"), std::string("Symbol(d)"));
  CHECK_EQ(str("var s = Symbol('d'); var v = Object.getPrototypeOf(Object(s)) === Symbol.prototype;"),
           std::string("true"));
  CHECK_EQ(str("var v = Symbol.prototype.constructor === Symbol;"), std::string("true"));
  CHECK_EQ(str("var s = Symbol('d'); var v = typeof Symbol.prototype[Symbol.toPrimitive];"),
           std::string("function"));
  // `Symbol()` has no description; `Symbol('')` has the empty one (20.4.3.2).
  CHECK_EQ(str("var v = String(Symbol().description);"), std::string("undefined"));
  CHECK_EQ(str("var v = JSON.stringify(Symbol('').description);"), std::string("\"\""));
}

BONK_TEST(js_symbol, constructor_errors) {
  CHECK(fails_with("new Symbol();", "not a constructor"));
  CHECK(fails_with("Symbol.keyFor('not a symbol');", "requires a symbol"));
  CHECK(fails_with("Symbol(Symbol('nested'));", "cannot convert a Symbol value to a string"));
  CHECK(fails_with("Symbol.prototype.valueOf.call({});", "non-symbol"));
}

BONK_TEST(js_symbol, numbers_and_tags) {
  CHECK_EQ(num("var v = Symbol.for.length + Symbol.keyFor.length;"), 2.0);
  CHECK_EQ(num("var v = Symbol.length;"), 0.0);
  CHECK_EQ(str("var v = Symbol.prototype[Symbol.toStringTag];"), std::string("Symbol"));
  CHECK_EQ(str("var v = Object.prototype.toString.call(Symbol('t'));"), std::string("[object Symbol]"));
  CHECK_EQ(str("var v = Object.prototype.toString.call(Math) + ' ' + Object.prototype.toString.call(JSON);"),
           std::string("[object Math] [object JSON]"));
  CHECK_EQ(str("var v = Object.getOwnPropertyDescriptor(Symbol, 'prototype').writable;"), std::string("false"));
}

// --------------------------------------------------------------- boxing ----

BONK_TEST(js_symbol, object_boxes_primitives_onto_their_prototypes) {
  CHECK_EQ(num("var v = Object('abc').length;"), 3.0);
  CHECK_EQ(str("var v = Object('abc')[1];"), std::string("b"));
  CHECK_EQ(str("var v = typeof Object(5);"), std::string("object"));
  CHECK_EQ(num("var v = Object(5).valueOf();"), 5.0);
  CHECK_EQ(str("var v = Object(true).valueOf();"), std::string("true"));
  CHECK_EQ(str("var v = Object.getPrototypeOf(Object('x')) === String.prototype;"), std::string("true"));
}
