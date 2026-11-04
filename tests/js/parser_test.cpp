// Parser-level behaviour through the public engine API: operator precedence,
// arrow functions, literals, statements and graceful rejection of unsupported syntax.
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

BONK_TEST(js_parser, multiplication_binds_tighter_than_addition) {
  CHECK_EQ(num("var v = 1 + 2 * 3;"), 7.0);
  CHECK_EQ(num("var v = (1 + 2) * 3;"), 9.0);
}

BONK_TEST(js_parser, comparison_and_logical_precedence) {
  CHECK_EQ(num("var v = 1 + 1 == 2 && 3 > 2 ? 10 : 20;"), 10.0);
  CHECK_EQ(num("var v = false || true && false ? 1 : 2;"), 2.0);
}

BONK_TEST(js_parser, assignment_is_right_associative) {
  CHECK_EQ(num("var a, b, c; a = b = c = 4; v = a + b + c;"), 12.0);
}

BONK_TEST(js_parser, compound_assignment_and_update) {
  CHECK_EQ(num("var v = 5; v += 2; v *= 3; v -= 1; v /= 2;"), 10.0);
  CHECK_EQ(num("var v = 1; v++; ++v; v--;"), 2.0);
}

BONK_TEST(js_parser, array_and_object_literals) {
  CHECK_EQ(num("var o = {a: 1, b: [2, 3]}; var v = o.a + o.b[0] + o.b[1];"), 6.0);
  CHECK_EQ(num("var v = [1, 2, 3].length;"), 3.0);
  CHECK_EQ(str("var key = 'x'; var o = {x: 'yes'}; var v = o[key];"), std::string("yes"));
}

BONK_TEST(js_parser, arrow_functions) {
  CHECK_EQ(num("var double = x => x * 2; var v = double(21);"), 42.0);
  CHECK_EQ(num("var add = (a, b) => a + b; var v = add(2, 3);"), 5.0);
  CHECK_EQ(num("var f = () => { return 7; }; var v = f();"), 7.0);
}

BONK_TEST(js_parser, nested_blocks_and_if_else_chains) {
  const std::string source =
      "var v = 0;\n"
      "if (1 > 2) { v = 1; } else if (2 > 1) { v = 2; } else { v = 3; }\n";
  CHECK_EQ(num(source), 2.0);
}

BONK_TEST(js_parser, switch_and_class_are_rejected_with_a_message) {
  Context context;
  const ExecutionResult switch_result = context.evaluate("switch (1) { case 1: break; }");
  CHECK(!switch_result.ok);
  CHECK(switch_result.message.find("switch") != std::string::npos);

  Context class_context;
  const ExecutionResult class_result = class_context.evaluate("class A {}");
  CHECK(!class_result.ok);
  CHECK(class_result.message.find("class") != std::string::npos);
}

BONK_TEST(js_parser, missing_operand_is_a_syntax_error) {
  Context context;
  const ExecutionResult result = context.evaluate("var v = 1 + ;");
  CHECK(!result.ok);
  CHECK(result.message.find("js.syntax") != std::string::npos);
  CHECK(!result.exception.is_undefined());
}

BONK_TEST(js_parser, unclosed_block_is_a_syntax_error) {
  Context context;
  const ExecutionResult result = context.evaluate("function f() { return 1;");
  CHECK(!result.ok);
}

BONK_TEST(js_parser, deep_expression_does_not_overflow) {
  std::string source = "var v = 1";
  for (int i = 0; i < 200; ++i) source += " + 1";
  source += ";";
  CHECK_EQ(num(source), 201.0);
}
