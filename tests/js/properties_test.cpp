// Property descriptors, Object.prototype and the Object statics (stage 23).
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

}  // namespace

BONK_TEST(js_properties, object_keys_lists_own_enumerable_properties) {
  CHECK_EQ(str("var o = {a: 1, b: 2}; var v = Object.keys(o).join(',');"), std::string("a,b"));
  CHECK_EQ(num("var o = {a: 1}; Object.defineProperty(o, 'hidden', {value: 2, enumerable: false});"
               "var v = Object.keys(o).length;"),
           1.0);
  CHECK_EQ(str("var o = {a: 1}; Object.defineProperty(o, 'h', {value: 2, enumerable: false});"
               "var v = Object.getOwnPropertyNames(o).join(',');"),
           std::string("a,h"));
  CHECK_EQ(num("var v = Object.keys([7, 8]).length;"), 2.0);
  CHECK_EQ(str("var v = Object.keys([7, 8]).join(',');"), std::string("0,1"));
}

BONK_TEST(js_properties, descriptors_report_attributes) {
  CHECK_EQ(num("var o = {a: 1}; var d = Object.getOwnPropertyDescriptor(o, 'a'); var v = d.value;"), 1.0);
  CHECK_EQ(str("var o = {a: 1}; var d = Object.getOwnPropertyDescriptor(o, 'a');"
               "var v = [d.writable, d.enumerable, d.configurable].join(',');"),
           std::string("true,true,true"));
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 'a', {value: 1, writable: false, enumerable: false,"
               "configurable: false}); var d = Object.getOwnPropertyDescriptor(o, 'a');"
               "var v = [d.writable, d.enumerable, d.configurable].filter(function (x) { return x; }).length;"),
           0.0);
  CHECK_EQ(str("var v = typeof Object.getOwnPropertyDescriptor({}, 'missing');"), std::string("undefined"));
}

BONK_TEST(js_properties, define_property_writes_and_rejects_redefinition) {
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 'a', {value: 42}); var v = o.a;"), 42.0);
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 'a', {value: 1, writable: false}); o.a = 2;"
               "var v = o.a;"),
           1.0);
  // Redefining a non-configurable, non-writable property is a TypeError.
  CHECK_EQ(str("var o = {}; Object.defineProperty(o, 'a', {value: 1, writable: false, configurable: false});\n"
               "try { Object.defineProperty(o, 'a', {value: 2}); var v = 'no throw'; }\n"
               "catch (e) { var v = e.name; }"),
           std::string("TypeError"));
  CHECK_EQ(str("var o = {}; delete o.missing; try { Object.defineProperty(o, 'x', {value: 1,"
               "configurable: false}); delete o.x; var v = o.hasOwnProperty('x') ? 'kept' : 'gone'; }\n"
               "catch (e) { var v = 'threw'; }"),
           std::string("kept"));
}

BONK_TEST(js_properties, accessors_see_their_receiver) {
  CHECK_EQ(num("var o = {_x: 5, get x() { return this._x; }, set x(value) { this._x = value; }};\n"
               "o.x = 9; var v = o.x;"),
           9.0);
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 'double', {get: function () { return 21 * 2; }});"
               "var v = o.double;"),
           42.0);
  CHECK_EQ(num("var store = {};\n"
               "Object.defineProperty(store, 'v', {set: function (value) { this.saved = value; }});\n"
               "store.v = 7; var v = store.saved;"),
           7.0);
}

BONK_TEST(js_properties, object_prototype_methods) {
  CHECK_EQ(num("var o = {a: 1}; var v = o.hasOwnProperty('a') ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var o = {a: 1}; var v = o.hasOwnProperty('toString') ? 1 : 0;"), 0.0);
  CHECK_EQ(num("var v = Object.prototype.hasOwnProperty.call({k: 1}, 'k') ? 1 : 0;"), 1.0);
  CHECK_EQ(str("var v = Object.prototype.toString.call([]);"), std::string("[object Array]"));
  CHECK_EQ(str("var v = Object.prototype.toString.call(null);"), std::string("[object Null]"));
  CHECK_EQ(str("var v = Object.prototype.toString.call(3);"), std::string("[object Number]"));
  CHECK_EQ(num("var o = {a: 1}; var count = 0; for (var key in o) { count = count + 1; } var v = count;"), 1.0);
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 'h', {value: 1, enumerable: false});"
               "var count = 0; for (var key in o) { count = count + 1; } var v = count;"),
           0.0);
}

BONK_TEST(js_properties, create_and_prototype_chain) {
  CHECK_EQ(num("var proto = {inherited: 7}; var o = Object.create(proto); var v = o.inherited;"), 7.0);
  CHECK_EQ(num("var proto = {inherited: 7}; var o = Object.create(proto);"
               "var v = o.hasOwnProperty('inherited') ? 1 : 0;"),
           0.0);
  CHECK_EQ(num("var proto = {inherited: 7}; var o = Object.create(proto); var v = proto.isPrototypeOf(o) ? 1 : 0;"),
           1.0);
  CHECK_EQ(str("var o = Object.create(null); var v = typeof o.hasOwnProperty;"), std::string("undefined"));
  CHECK_EQ(str("var v = typeof Object.getPrototypeOf(Object.create(null));"), std::string("object"));
  CHECK_EQ(num("var v = Object.getPrototypeOf({}) === Object.prototype ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var o = Object.create({base: 1}, {own: {value: 2, enumerable: true}});"
               "var v = o.own + o.base;"),
           3.0);
}

BONK_TEST(js_properties, freeze_and_seal_block_writes) {
  CHECK_EQ(num("var o = Object.freeze({x: 1}); o.x = 9; var v = o.x;"), 1.0);
  CHECK_EQ(num("var o = Object.freeze({x: 1}); var v = Object.isFrozen(o) ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var o = {x: 1}; var v = Object.isFrozen(o) ? 1 : 0;"), 0.0);
  CHECK_EQ(str("var o = Object.seal({x: 1}); o.y = 2; var v = typeof o.y;"), std::string("undefined"));
  CHECK_EQ(str("var o = {}; Object.preventExtensions(o); o.late = 1; var v = typeof o.late;"),
           std::string("undefined"));
  CHECK_EQ(num("var o = {}; var v = Object.isExtensible(o) ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var o = Object.freeze({a: 1, b: 2}); var v = Object.keys(o).length;"), 2.0);
}

BONK_TEST(js_properties, object_is_and_assign) {
  CHECK_EQ(num("var v = Object.is(NaN, NaN) ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = Object.is(0, -0) ? 1 : 0;"), 0.0);
  CHECK_EQ(num("var v = Object.is(1, 1) ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var target = {a: 1}; Object.assign(target, {b: 2}, {c: 3});"
               "var v = target.a + target.b + target.c;"),
           6.0);
  CHECK_EQ(str("var v = Object.entries({a: 1, b: 2}).map(function (p) { return p[0] + p[1]; }).join('');"),
           std::string("a1b2"));
  CHECK_EQ(str("var v = Object.values({a: 1, b: 2}).join(',');"), std::string("1,2"));
}

BONK_TEST(js_properties, functions_are_objects_too) {
  CHECK_EQ(num("function f() {} f.custom = 1; var v = f.custom;"), 1.0);
  CHECK_EQ(num("function f() {} Object.defineProperty(f, 'extra', {value: 2}); var v = f.extra;"), 2.0);
  CHECK_EQ(num("function f() {} var v = f.hasOwnProperty('custom') ? 1 : 0;"), 0.0);
  CHECK_EQ(num("function f() {} var v = f.prototype.constructor === f ? 1 : 0;"), 1.0);
  CHECK_EQ(str("var v = typeof Object.keys(function f() {}).length;"), std::string("number"));
}

BONK_TEST(js_properties, array_constructor_statics) {
  CHECK_EQ(num("var v = Array.isArray([]) ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = Array.isArray({length: 0}) ? 1 : 0;"), 0.0);
  CHECK_EQ(num("var v = Array(3).length;"), 3.0);
  CHECK_EQ(num("var v = Array(1, 2, 3).length;"), 3.0);
}

BONK_TEST(js_properties, non_writable_and_non_extensible_do_not_throw_in_sloppy_code) {
  Context context;
  const ExecutionResult result = context.evaluate(
      "var o = {};\n"
      "Object.defineProperty(o, 'fixed', {value: 1, writable: false, configurable: false});\n"
      "o.fixed = 99;\n"
      "Object.preventExtensions(o);\n"
      "o.fresh = 1;\n"
      "var v = o.fixed;");
  CHECK(result.ok);
  CHECK_EQ(to_number(context.runtime().global("v")), 1.0);
}
