// Array.prototype, array-likes, the arguments object and direct eval (stage 24).
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
  if (result.ok) return false;
  return result.message.find(needle) != std::string::npos || result.message.find(needle) != std::string::npos;
}

}  // namespace

BONK_TEST(js_arrays, array_prototype_holds_the_methods) {
  CHECK_EQ(num("var a = [1, 2]; var v = a.push(3);"), 3.0);
  CHECK_EQ(str("var a = [1, 2]; a.push(3); var v = a.join('-');"), std::string("1-2-3"));
  CHECK_EQ(num("var a = [1, 2, 3]; var v = a.pop();"), 3.0);
  CHECK_EQ(num("var a = [1, 2, 3]; var v = a.shift();"), 1.0);
  CHECK_EQ(str("var a = [2, 3]; a.unshift(1); var v = a.join(',');"), std::string("1,2,3"));
  CHECK_EQ(str("var v = [].pop();"), std::string("undefined"));
}

BONK_TEST(js_arrays, search_and_transform) {
  CHECK_EQ(num("var v = [10, 20, 30].indexOf(20);"), 1.0);
  CHECK_EQ(num("var v = [10, 20, 20].lastIndexOf(20);"), 2.0);
  CHECK(num("[1, NaN].includes(NaN)"));
  CHECK(!bool(to_boolean(eval_global("[1, 2].includes(3)", "v"))));
  CHECK_EQ(str("var v = [1, 2, 3].map(function (x) { return x * 2; }).join(',');"), std::string("2,4,6"));
  CHECK_EQ(str("var v = [1, 2, 3, 4].filter(function (x) { return x % 2 === 0; }).join(',');"), std::string("2,4"));
  CHECK_EQ(num("var v = [1, 2, 3].reduce(function (a, b) { return a + b; }, 10);"), 16.0);
  CHECK_EQ(num("var v = [1, 2, 3].reduce(function (a, b) { return a + b; });"), 6.0);
  CHECK_EQ(num("var v = [1, 2, 3].find(function (x) { return x > 1; });"), 2.0);
  CHECK_EQ(num("var v = [1, 2, 3].findIndex(function (x) { return x > 1; });"), 1.0);
  CHECK(num("[1, 2].every(function (x) { return x > 0; })"));
  CHECK(bool(to_boolean(eval_global("var v = [1, 2].some(function (x) { return x > 1; });", "v"))));
  CHECK_EQ(str("var v = [3, 1, 2].sort().join(',');"), std::string("1,2,3"));
  CHECK_EQ(str("var v = [10, 9].sort(function (a, b) { return a - b; }).join(',');"), std::string("9,10"));
  CHECK_EQ(str("var v = [1, 2, 3].reverse().join(',');"), std::string("3,2,1"));
}

BONK_TEST(js_arrays, slice_splice_and_concat) {
  CHECK_EQ(str("var v = [1, 2, 3, 4].slice(1, 3).join(',');"), std::string("2,3"));
  CHECK_EQ(str("var v = [1, 2, 3].slice(-1).join(',');"), std::string("3"));
  CHECK_EQ(str("var a = [1, 2, 3]; var removed = a.splice(1, 1); var v = removed.join(',') + '/' + a.join(',');"),
           std::string("2/1,3"));
  CHECK_EQ(str("var a = [1, 4]; a.splice(1, 0, 2, 3); var v = a.join(',');"), std::string("1,2,3,4"));
  CHECK_EQ(str("var v = [1].concat([2, 3], 4).join(',');"), std::string("1,2,3,4"));
  CHECK_EQ(str("var v = [1, 2, 3].toString();"), std::string("1,2,3"));
}

BONK_TEST(js_arrays, methods_work_on_array_likes) {
  // The generic form is what `arguments` and strings rely on.
  CHECK_EQ(str("function f() { return Array.prototype.slice.call(arguments).join('|'); }\n"
               "var v = f(1, 2, 3);"),
           std::string("1|2|3"));
  CHECK_EQ(str("function f() { return Array.prototype.join.call(arguments, '-'); }\n"
               "var v = f('a', 'b');"),
           std::string("a-b"));
  CHECK_EQ(str("var v = Array.prototype.map.call('abc', function (c) { return c.toUpperCase(); }).join('');"),
           std::string("ABC"));
  CHECK_EQ(num("var v = Array.prototype.indexOf.call({0: 'x', 1: 'y', length: 2}, 'y');"), 1.0);
}

BONK_TEST(js_arrays, arrays_are_instances_of_array) {
  CHECK(num("var v = [] instanceof Array"));
  CHECK(num("var v = Array.isArray([])"));
  CHECK(!bool(to_boolean(eval_global("var v = Array.isArray({})", "v"))));
  CHECK(num("var v = Array.prototype.isPrototypeOf([])"));
  CHECK(num("var v = [].constructor === Array"));
  CHECK_EQ(num("var v = Array(3).length;"), 3.0);
}

BONK_TEST(js_arrays, arguments_object_basics) {
  CHECK_EQ(num("function f(a, b) { return arguments.length; } var v = f(1, 2, 3);"), 3.0);
  CHECK_EQ(num("function f(a, b) { return arguments[1]; } var v = f(1, 2);"), 2.0);
  CHECK(num("function f() { return arguments.callee === f; } var v = f()"));
  CHECK_EQ(str("function f() { var count = 0; for (var key in arguments) { count = count + 1; } return count; }\n"
               "var v = f('a', 'b');"),
           std::string("2"));
}

BONK_TEST(js_arrays, arguments_aliases_parameters_in_sloppy_code) {
  // Writing through the arguments object updates the parameter…
  CHECK_EQ(str("function f(p) { arguments[0] = 'changed'; return p; } var v = f('original');"),
           std::string("changed"));
  // …and writing the parameter is visible through the arguments object.
  CHECK_EQ(str("function f(q) { q = 'changed'; return arguments[0]; } var v = f('original');"),
           std::string("changed"));
  // Extra actual arguments are not mapped.
  CHECK_EQ(num("function f(a) { arguments[1] = 5; return arguments.length; } var v = f(1, 2);"), 2.0);
  CHECK_EQ(num("function f(a) { return f2(arguments); } function f2(a) { return a.length; } var v = f1 ? 0 : 1;"),
           1.0);
}

BONK_TEST(js_arrays, eval_runs_in_the_calling_scope) {
  CHECK_EQ(num("var x = 1; var v = eval('x + 1');"), 2.0);
  CHECK_EQ(num("eval('var y = 5;'); var v = y;"), 5.0);
  CHECK_EQ(num("function f() { var local = 41; return eval('local + 1'); } var v = f();"), 42.0);
  CHECK_EQ(str("var v = eval('({a: 7}).a');"), std::string("7"));
  CHECK_EQ(str("var v = typeof eval(42);"), std::string("number"));
  CHECK(fails_with("eval('var = ;');", "SyntaxError"));
  CHECK(fails_with("eval('1 = 2;');", "SyntaxError"));
}

BONK_TEST(js_arrays, invalid_assignment_targets_are_early_errors) {
  CHECK(fails_with("1 = 2;", "js.syntax"));
  CHECK(fails_with("this = 1;", "js.syntax"));
  CHECK(fails_with("function f() {} f() = 1;", "js.syntax"));
  CHECK(fails_with("var a, b; (a, b) = 1;", "js.syntax"));
  CHECK(fails_with("var a = 1; a++ = 2;", "js.syntax"));
  CHECK(fails_with("'x' += 1;", "js.syntax"));
  // Valid targets still parse.
  CHECK_EQ(num("var o = {}; o.x = 5; o['y'] = 6; var v = o.x + o.y;"), 11.0);
  CHECK_EQ(num("var a = 1; a += 2; a++; var v = a;"), 4.0);
}
