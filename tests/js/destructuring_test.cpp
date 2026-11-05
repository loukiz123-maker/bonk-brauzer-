// Spread, rest and destructuring (stage 30).
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

BONK_TEST(js_destructuring, array_patterns_bind_in_order) {
  CHECK_EQ(str("var [a, b] = [1, 2]; var v = a + ',' + b;"), std::string("1,2"));
  CHECK_EQ(num("var [a, , c] = [1, 2, 3]; var v = a + c;"), 4.0);
  CHECK_EQ(num("var [a, b = 10] = [1]; var v = a + b;"), 11.0);
  CHECK_EQ(num("var [a, [b, c]] = [1, [2, 3]]; var v = a + b + c;"), 6.0);
  CHECK_EQ(num("var [a] = 'xy'; var v = a.length;"), 1.0);
}

BONK_TEST(js_destructuring, object_patterns_read_own_properties) {
  CHECK_EQ(str("var {a, b: c} = {a: 1, b: 2}; var v = a + ',' + c;"), std::string("1,2"));
  CHECK_EQ(num("var {a = 5} = {}; var v = a;"), 5.0);
  CHECK_EQ(num("var {a: {b}} = {a: {b: 7}}; var v = b;"), 7.0);
  CHECK_EQ(num("var o = {x: 1}; var {x} = o; var v = x;"), 1.0);
}

BONK_TEST(js_destructuring, pattern_initialisers_are_evaluated_once) {
  CHECK_EQ(num("var calls = 0; function make() { calls++; return [1, 2]; }"
               "var [a, b] = make(); var v = calls * 10 + a + b;"),
           13.0);
  CHECK_EQ(num("var calls = 0; var o = {get p() { calls++; return 1; }};"
               "var {p} = o; var v = calls * 10 + p;"),
           11.0);
}

BONK_TEST(js_destructuring, assignment_patterns_write_existing_bindings) {
  CHECK_EQ(str("var a, b; [a, b] = [1, 2]; var v = a + ',' + b;"), std::string("1,2"));
  CHECK_EQ(num("var o = {}; [o.x, o.y] = [3, 4]; var v = o.x * 10 + o.y;"), 34.0);
  CHECK_EQ(num("var a; var r = ([a] = [7]); var v = r[0] + a;"), 14.0);
}

BONK_TEST(js_destructuring, rest_elements_and_rest_properties) {
  CHECK_EQ(str("var [a, ...rest] = [1, 2, 3]; var v = a + ':' + rest.join('-');"), std::string("1:2-3"));
  CHECK_EQ(str("var {a, ...others} = {a: 1, b: 2, c: 3}; var v = JSON.stringify(others);"),
           std::string("{\"b\":2,\"c\":3}"));
  CHECK_EQ(num("var {...all} = {x: 1, y: 2}; var v = Object.keys(all).length;"), 2.0);
}

BONK_TEST(js_destructuring, rest_parameters_collect_extra_arguments) {
  CHECK_EQ(num("function f(a, ...rest) { return rest.length; } var v = f(1, 2, 3);"), 2.0);
  CHECK_EQ(str("function f(...all) { return all.join('-'); } var v = f(1, 2, 3);"),
           std::string("1-2-3"));
  CHECK_EQ(num("function f(a, ...rest) { return rest.length; } var v = f(1) + f(1, 2);"), 1.0);
}

BONK_TEST(js_destructuring, default_parameters) {
  CHECK_EQ(num("function f(a = 5) { return a; } var v = f() + f(2);"), 7.0);
  CHECK_EQ(num("function f(a, b = a * 2) { return b; } var v = f(3);"), 6.0);
  CHECK_EQ(num("function f({a} = {a: 9}) { return a; } var v = f();"), 9.0);
}

BONK_TEST(js_destructuring, spread_in_arrays_calls_and_objects) {
  CHECK_EQ(str("var a = [1, 2]; var v = [...a, 3].join(',');"), std::string("1,2,3"));
  CHECK_EQ(str("var v = [...'ab', 'c'].join('');"), std::string("abc"));
  CHECK_EQ(num("function f(x, y, z) { return x + y + z; }"
               "var v = f(...[1, 2], 3);"),
           6.0);
  CHECK_EQ(num("var v = Math.max(...[3, 9, 4]);"), 9.0);
  CHECK_EQ(str("var o = {a: 1}; var p = {...o, b: 2}; var v = JSON.stringify(p);"),
           std::string("{\"a\":1,\"b\":2}"));
  CHECK_EQ(num("var v = Object.keys({...null, ...undefined, x: 1}).length;"), 1.0);
}

BONK_TEST(js_destructuring, patterns_in_parameters_and_loops) {
  CHECK_EQ(str("function f({x}, [y]) { return x + y; } var v = f({x: 1}, [2]) + '';"),
           std::string("3"));
  CHECK_EQ(num("var sum = 0; for (var [k, v] of [[1, 2], [3, 4]]) sum += k + v; var total = sum;",
               "total"),
           10.0);
  CHECK_EQ(num("var n = 0; for (var {x} of [{x: 1}, {x: 2}]) n += x; var v = n;"), 3.0);
}

BONK_TEST(js_destructuring, spreading_a_primitive_is_a_type_error) {
  CHECK(fails_with("var a = [...5];", "not iterable"));
  CHECK(fails_with("var a = [...undefined];", "not iterable"));
}
