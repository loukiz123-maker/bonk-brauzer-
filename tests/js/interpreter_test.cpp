// Interpreter semantics: scopes, closures, recursion, loops, exceptions,
// `this`, constructors and coercion rules.
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

BONK_TEST(js_interpreter, closures_capture_their_scope) {
  const std::string source =
      "function counter() { var n = 0; return function() { n = n + 1; return n; }; }\n"
      "var c = counter(); c(); c(); var v = c();";
  CHECK_EQ(num(source), 3.0);
}

BONK_TEST(js_interpreter, recursion) {
  const std::string source =
      "function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }\n"
      "var v = fact(6);";
  CHECK_EQ(num(source), 720.0);
}

BONK_TEST(js_interpreter, loops_and_break_continue) {
  const std::string source =
      "var total = 0;\n"
      "for (var i = 0; i < 10; i++) { if (i % 2 === 0) continue; if (i > 7) break; total += i; }\n"
      "var v = total;";
  CHECK_EQ(num(source), 1.0 + 3 + 5 + 7);
}

BONK_TEST(js_interpreter, while_and_do_while) {
  CHECK_EQ(num("var i = 0; while (i < 4) i++; var v = i;"), 4.0);
  CHECK_EQ(num("var i = 0; do { i++; } while (i < 3); var v = i;"), 3.0);
}

BONK_TEST(js_interpreter, for_of_and_for_in) {
  CHECK_EQ(num("var sum = 0; for (var x of [1, 2, 3]) sum += x; var v = sum;"), 6.0);
  CHECK_EQ(str("var letters = ''; for (var c of 'abc') letters += c; var v = letters;"), std::string("abc"));
  CHECK_EQ(num("var n = 0; var o = {a: 1, b: 2}; for (var k in o) n++; var v = n;"), 2.0);
}

BONK_TEST(js_interpreter, try_catch_finally) {
  const std::string source =
      "var log = '';\n"
      "try { log += 'try'; throw new Error('boom'); } catch (e) { log += ':' + e.message; } "
      "finally { log += ':finally'; }\n"
      "var v = log;";
  CHECK_EQ(str(source), std::string("try:boom:finally"));
}

BONK_TEST(js_interpreter, finally_runs_even_without_a_throw) {
  CHECK_EQ(str("var v = ''; try { v = 'a'; } finally { v += 'b'; }"), std::string("ab"));
}

BONK_TEST(js_interpreter, thrown_value_propagates_out_of_a_function) {
  Context context;
  const ExecutionResult result = context.evaluate(
      "function boom() { throw 'nope'; }\n"
      "boom();\n");
  CHECK(!result.ok);
  CHECK_EQ(describe_exception(result.exception), std::string("nope"));
}

BONK_TEST(js_interpreter, this_inside_methods_and_constructors) {
  CHECK_EQ(num(
               "var obj = {value: 41, bump: function() { return this.value + 1; }};\n"
               "var v = obj.bump();"),
           42.0);
  CHECK_EQ(num(
               "function Point(x) { this.x = x; }\n"
               "Point.prototype.get = function() { return this.x; };\n"
               "var v = new Point(9).get();"),
           9.0);
}

BONK_TEST(js_interpreter, prototypes_and_instanceof) {
  CHECK_EQ(num(
               "function A() {}\n"
               "var a = new A();\n"
               "var v = (a instanceof A) ? 1 : 0;"),
           1.0);
  CHECK_EQ(num("var v = ({} instanceof Object) ? 1 : 0;"), 0.0);
}

BONK_TEST(js_interpreter, typeof_and_coercions) {
  CHECK_EQ(str("var v = typeof undefined;"), std::string("undefined"));
  CHECK_EQ(str("var v = typeof 1;"), std::string("number"));
  CHECK_EQ(str("var v = typeof 'x';"), std::string("string"));
  CHECK_EQ(str("var v = typeof function(){};"), std::string("function"));
  CHECK_EQ(str("var v = typeof null;"), std::string("object"));
  CHECK_EQ(str("var v = '5' + 1;"), std::string("51"));
  CHECK_EQ(num("var v = '5' * 2;"), 10.0);
  CHECK_EQ(num("var v = true == 1 ? 1 : 0;"), 1.0);
  CHECK_EQ(num("var v = (null === undefined) ? 1 : 0;"), 0.0);
  CHECK_EQ(num("var v = (null == undefined) ? 1 : 0;"), 1.0);
}

BONK_TEST(js_interpreter, delete_and_in) {
  CHECK_EQ(num("var o = {a: 1}; delete o.a; var v = ('a' in o) ? 1 : 0;"), 0.0);
  CHECK_EQ(num("var a = [1, 2]; var v = (1 in a) ? 1 : 0;"), 1.0);
}

BONK_TEST(js_interpreter, let_and_const_are_block_scoped) {
  CHECK_EQ(num("let v = 1; { let v = 2; } "), 1.0);
  CHECK_EQ(num("const v = 3;"), 3.0);
}
