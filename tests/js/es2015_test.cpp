// ES2015-era gaps closed in stage 28: number formatting, `Function`, `__proto__`,
// new Array methods, primitive receivers and huge array lengths.
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

BONK_TEST(js_es2015, number_to_string_uses_the_smallest_round_trip_form) {
  CHECK_EQ(str("var v = String(0.000001);"), std::string("0.000001"));
  CHECK_EQ(str("var v = String(0.0000001);"), std::string("1e-7"));
  CHECK_EQ(str("var v = String(1e21);"), std::string("1e+21"));
  CHECK_EQ(str("var v = String(1e20);"), std::string("100000000000000000000"));
  CHECK_EQ(str("var v = String(1 / 3);"), std::string("0.3333333333333333"));
  CHECK_EQ(str("var v = String(0.1 + 0.2);"), std::string("0.30000000000000004"));
  CHECK_EQ(str("var v = JSON.stringify({a: 1e-7});"), std::string("{\"a\":1e-7}"));
}

BONK_TEST(js_es2015, numeric_property_keys_use_the_same_formatting) {
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 0.000001, {value: 1});"
               "var v = o.hasOwnProperty('0.000001') ? 1 : 0;"),
           1.0);
  CHECK_EQ(num("var v = ({1e-7: 2})['1e-7'];"), 2.0);
}

BONK_TEST(js_es2015, function_constructor_parses_its_body) {
  CHECK_EQ(num("var f = new Function('a', 'b', 'return a + b'); var v = f(2, 3);"), 5.0);
  CHECK_EQ(num("var v = new Function('return 7')();"), 7.0);
  CHECK_EQ(num("var f = new Function('a', 'b', 'return 0'); var v = f.length;"), 2.0);
  CHECK_EQ(num("var v = (function () {}).constructor === Function ? 1 : 0;"), 1.0);
  CHECK(fails_with("new Function('return )');", "SyntaxError"));
}

BONK_TEST(js_es2015, constructors_are_reachable_from_values) {
  CHECK_EQ(num("var v = [].constructor === Array ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = ({}).constructor === Object ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = 'x'.constructor === String ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = (5).constructor === Number ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = true.constructor === Boolean ? 1 : 0;"), 1.0);
}

BONK_TEST(js_es2015, proto_accessor_links_an_object_to_a_new_prototype) {
  CHECK_EQ(str("var base = {greet: 'hi'}; var o = {}; o.__proto__ = base; var v = o.greet;"), std::string("hi"));
  CHECK_EQ(num("var base = {greet: 'hi'}; var o = {}; o.__proto__ = base; var v = o.__proto__ === base;"), 1.0);
  CHECK_EQ(num("var o = {}; var v = o.__proto__ === Object.prototype;"), 1.0);
}

BONK_TEST(js_es2015, new_array_methods) {
  CHECK_EQ(num("var v = [1, 2, 3].at(-1);"), 3.0);
  CHECK_EQ(str("var v = [1, 2, 3, 4, 5].copyWithin(0, 3).join(',');"), std::string("4,5,3,4,5"));
  CHECK_EQ(num("var v = [1, [2, [3, [4]]]].flat(2).length;"), 4.0);
  CHECK_EQ(str("var v = [1, 2].flatMap(function (x) { return [x, x * 2]; }).join(',');"),
           std::string("1,2,2,4"));
  CHECK_EQ(num("var v = [1, 2, 3, 4].findLast(function (x) { return x % 2 === 0; });"), 4.0);
  CHECK_EQ(num("var v = [1, 2, 3, 4].findLastIndex(function (x) { return x % 2 === 1; });"), 2.0);
  CHECK_EQ(str("var v = [1, null, 2].toLocaleString();"), std::string("1,,2"));
}

BONK_TEST(js_es2015, primitives_are_empty_array_likes) {
  CHECK_EQ(num("var v = Array.prototype.push.call(true);"), 0.0);
  CHECK_EQ(num("var v = Array.prototype.map.call(1, function (x) { return x; }).length;"), 0.0);
}

BONK_TEST(js_es2015, huge_array_lengths_do_not_allocate) {
  CHECK_EQ(num("var a = []; a.length = 4294967295; var v = a.length;"), 4294967295.0);
  CHECK(fails_with("var a = []; a.length = 4294967295; a.join('-');", "RangeError"));
  CHECK(fails_with("var v = 'ab'.repeat(1e9);", "RangeError"));
  CHECK(fails_with("var a = []; a.length = 4294967296;", "RangeError"));
  CHECK_EQ(num("var a = [1, 2, 3]; a.length = 1; var v = a.length;"), 1.0);
}

BONK_TEST(js_es2015, unresolved_reads_throw_reference_errors) {
  CHECK(fails_with("nope;", "is not defined"));
  CHECK(fails_with("nope *= 2;", "is not defined"));
  CHECK_EQ(str("var v = typeof nope;"), std::string("undefined"));
  CHECK_EQ(num("undeclared = 5; var v = undeclared;"), 5.0);
}
