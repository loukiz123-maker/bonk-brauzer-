// String/Number/Boolean prototypes, boxed wrappers and the bitwise compound
// assignment operators (stage 25).
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

BONK_TEST(js_primitives, string_prototype_carries_the_methods) {
  CHECK_EQ(str("var v = '  x  '.trim();"), std::string("x"));
  CHECK_EQ(str("var v = 'abc'.toUpperCase();"), std::string("ABC"));
  CHECK_EQ(str("var v = 'ABC'.toLowerCase();"), std::string("abc"));
  CHECK_EQ(str("var v = 'abcdef'.slice(1, 3);"), std::string("bc"));
  CHECK_EQ(str("var v = 'abcdef'.slice(-2);"), std::string("ef"));
  CHECK_EQ(str("var v = 'abcdef'.substring(4, 1);"), std::string("bcd"));
  CHECK_EQ(str("var v = 'abcdef'.substr(-2);"), std::string("ef"));
  CHECK_EQ(str("var v = String.prototype.trim.call('  y  ');"), std::string("y"));
  CHECK_EQ(str("var v = ''.concat('a', 'b', 1);"), std::string("ab1"));
}

BONK_TEST(js_primitives, string_search_and_split) {
  CHECK_EQ(num("var v = 'abcabc'.indexOf('c');"), 2.0);
  CHECK_EQ(num("var v = 'abcabc'.indexOf('c', 3);"), 5.0);
  CHECK_EQ(num("var v = 'abcabc'.lastIndexOf('b');"), 4.0);
  CHECK(num("var v = 'abc'.includes('bc')"));
  CHECK(num("var v = 'abc'.startsWith('ab')"));
  CHECK(num("var v = 'abc'.endsWith('bc')"));
  CHECK_EQ(str("var v = 'a,b,c'.split(',').join('-');"), std::string("a-b-c"));
  CHECK_EQ(num("var v = 'abc'.split('').length;"), 3.0);
  CHECK_EQ(str("var v = 'a-b-c'.split('-', 2).join('|');"), std::string("a|b"));
  CHECK_EQ(str("var v = 'hello'.replace('l', 'L');"), std::string("heLlo"));
  CHECK_EQ(str("var v = 'hello'.replace('l', function (m) { return m.toUpperCase(); });"), std::string("heLlo"));
}

BONK_TEST(js_primitives, string_padding_and_repeat) {
  CHECK_EQ(str("var v = '5'.padStart(3, '0');"), std::string("005"));
  CHECK_EQ(str("var v = '5'.padEnd(3, '0');"), std::string("500"));
  CHECK_EQ(str("var v = 'ab'.repeat(3);"), std::string("ababab"));
  CHECK_EQ(str("var v = 'abc'.charAt(1);"), std::string("b"));
  CHECK_EQ(num("var v = 'abc'.charCodeAt(0);"), 97.0);
  CHECK_EQ(num("var v = 'abc'.length;"), 3.0);
  CHECK_EQ(str("var v = 'abc'[1];"), std::string("b"));
}

BONK_TEST(js_primitives, numbers_use_number_prototype) {
  CHECK_EQ(str("var v = (255).toString(16);"), std::string("ff"));
  CHECK_EQ(str("var v = (10).toString(2);"), std::string("1010"));
  CHECK_EQ(str("var v = (3.14159).toFixed(2);"), std::string("3.14"));
  CHECK_EQ(str("var v = (1234.5678).toPrecision(6);"), std::string("1234.57"));
  CHECK_EQ(str("var v = Number.prototype.toFixed.call(1.005, 1);"), std::string("1.0"));
  CHECK_EQ(num("var v = (42).valueOf();"), 42.0);
  CHECK_EQ(num("var v = Number.MAX_SAFE_INTEGER;"), 9007199254740991.0);
  CHECK(num("var v = Number.isInteger(5)"));
  CHECK(num("var v = Number.isNaN(NaN)"));
  CHECK(!bool(to_boolean(eval_global("var v = Number.isFinite(Infinity);", "v"))));
}

BONK_TEST(js_primitives, booleans_use_boolean_prototype) {
  CHECK_EQ(str("var v = true.toString();"), std::string("true"));
  CHECK_EQ(str("var v = false.toString();"), std::string("false"));
  CHECK(num("var v = true.valueOf()"));
  CHECK_EQ(str("var v = Boolean.prototype.toString.call(true);"), std::string("true"));
  CHECK(!bool(to_boolean(eval_global("var v = Boolean(false);", "v"))));
}

BONK_TEST(js_primitives, wrappers_box_the_primitive) {
  CHECK_EQ(str("var b = new String('boxed'); var v = typeof b;"), std::string("object"));
  CHECK_EQ(str("var b = new String('boxed'); var v = b.valueOf();"), std::string("boxed"));
  CHECK_EQ(num("var b = new String('boxed'); var v = b.length;"), 5.0);
  CHECK_EQ(str("var b = new String('boxed'); var v = Object.prototype.toString.call(b);"),
           std::string("[object String]"));
  CHECK_EQ(num("var n = new Number(41); var v = n + 1;"), 42.0);
  CHECK_EQ(str("var n = new Number(41); var v = typeof n;"), std::string("object"));
  CHECK_EQ(str("var n = new Number(41); var v = Object.prototype.toString.call(n);"), std::string("[object Number]"));
  CHECK_EQ(str("var b = new Boolean(true); var v = Object.prototype.toString.call(b);"),
           std::string("[object Boolean]"));
  CHECK_EQ(str("var v = typeof String('x');"), std::string("string"));
}

BONK_TEST(js_primitives, string_statics) {
  CHECK_EQ(str("var v = String.fromCharCode(72, 105);"), std::string("Hi"));
  CHECK_EQ(str("var v = String.fromCodePoint(65, 66);"), std::string("AB"));
  CHECK_EQ(num("var v = String.fromCharCode(65).charCodeAt(0);"), 65.0);
}

BONK_TEST(js_primitives, primitive_constructors_are_reachable_from_values) {
  CHECK(num("var v = 'abc'.constructor === String"));
  CHECK(num("var v = (1).constructor === Number"));
  CHECK(num("var v = true.constructor === Boolean"));
  CHECK(num("var v = String.prototype.isPrototypeOf(new String('x'))"));
  CHECK(!bool(to_boolean(eval_global("var v = String.prototype.isPrototypeOf('x');", "v"))));
}

BONK_TEST(js_primitives, bitwise_compound_assignment) {
  CHECK_EQ(num("var x = 1; x <<= 3; var v = x;"), 8.0);
  CHECK_EQ(num("var x = 8; x >>= 2; var v = x;"), 2.0);
  CHECK_EQ(num("var x = -1; x >>>= 28; var v = x;"), 15.0);
  CHECK_EQ(num("var x = 12; x &= 10; var v = x;"), 8.0);
  CHECK_EQ(num("var x = 12; x |= 3; var v = x;"), 15.0);
  CHECK_EQ(num("var x = 12; x ^= 5; var v = x;"), 9.0);
  CHECK_EQ(num("var x = 2; x **= 10; var v = x;"), 1024.0);
  CHECK_EQ(num("var o = {n: 1}; o.n <<= 4; var v = o.n;"), 16.0);
  CHECK_EQ(str("var s = 'a'; s += 'b'; var v = s;"), std::string("ab"));
}

BONK_TEST(js_primitives, unsigned_shift_and_toint32) {
  CHECK_EQ(num("var v = 5 >>> 1;"), 2.0);
  CHECK_EQ(num("var v = -1 >>> 0;"), 4294967295.0);
  CHECK_EQ(num("var v = -8 >> 1;"), -4.0);
  CHECK_EQ(num("var v = 1 << 31;"), -2147483648.0);
  CHECK_EQ(num("var v = 0.9 | 0;"), 0.0);
  CHECK_EQ(num("var v = NaN | 0;"), 0.0);
  CHECK_EQ(num("var v = Infinity | 0;"), 0.0);
}
