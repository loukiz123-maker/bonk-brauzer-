// Lexer-level behaviour, exercised through the public engine API: literals,
// comments, escapes, template literals and syntax errors.
#include <string>

#include "test_framework.hpp"

#include "bonk/js/js.hpp"

using namespace bonk;
using namespace bonk::js;

namespace {

Value evaluate_globals(const std::string& source, const std::string& name) {
  Context context;
  const ExecutionResult result = context.evaluate(source);
  CHECK(result.ok);
  if (!result.ok) return Value::undefined();
  return context.runtime().global(name);
}

double number_of_var(const std::string& source, const std::string& name) {
  return to_number(evaluate_globals(source, name));
}

std::string string_of_var(const std::string& source, const std::string& name) {
  return to_string(evaluate_globals(source, name));
}

}  // namespace

BONK_TEST(js_lexer, numeric_literals) {
  CHECK_EQ(number_of_var("var v = 42;", "v"), 42.0);
  CHECK_EQ(number_of_var("var v = 3.5;", "v"), 3.5);
  CHECK_EQ(number_of_var("var v = 1e3;", "v"), 1000.0);
  CHECK_EQ(number_of_var("var v = 0x1f;", "v"), 31.0);
  CHECK_EQ(number_of_var("var v = 0.5 + 0.25;", "v"), 0.75);
}

BONK_TEST(js_lexer, string_escapes) {
  CHECK_EQ(string_of_var("var v = 'a\\tb';", "v"), std::string("a\tb"));
  CHECK_EQ(string_of_var("var v = \"line\\nbreak\";", "v"), std::string("line\nbreak"));
  CHECK_EQ(string_of_var("var v = '\\u0041';", "v"), std::string("A"));
  CHECK_EQ(string_of_var("var v = 'quote\\'inside';", "v"), std::string("quote'inside"));
}

BONK_TEST(js_lexer, block_and_line_comments) {
  CHECK_EQ(number_of_var(
               "// one\n"
               "var a = 1; /* two\n"
               "   three */ var b = 2; var v = a + b;",
               "v"),
           3.0);
}

BONK_TEST(js_lexer, template_literals) {
  CHECK_EQ(string_of_var(
               "var name = 'world';\n"
               "var v = `hello ${name}, ${1 + 1} times`;",
               "v"),
           std::string("hello world, 2 times"));
}

BONK_TEST(js_lexer, template_literals_with_multiline_text) {
  CHECK_EQ(string_of_var("var v = `a\nb`;", "v"), std::string("a\nb"));
}

BONK_TEST(js_lexer, unterminated_string_is_a_syntax_error) {
  Context context;
  const ExecutionResult result = context.evaluate("var a = 'oops;\n");
  CHECK(!result.ok);
  CHECK(!result.message.empty());
}

BONK_TEST(js_lexer, unterminated_template_is_a_syntax_error) {
  Context context;
  const ExecutionResult result = context.evaluate("var a = `oops;\n");
  CHECK(!result.ok);
}

BONK_TEST(js_lexer, unterminated_block_comment_is_a_syntax_error) {
  Context context;
  const ExecutionResult result = context.evaluate("var a = 1; /* oops\n");
  CHECK(!result.ok);
  CHECK(result.message.find("comment") != std::string::npos);
}

BONK_TEST(js_lexer, automatic_semicolon_insertion) {
  const std::string source =
      "var a = 1\n"
      "var b = 2\n"
      "var sum = a + b\n";
  CHECK_EQ(number_of_var(source, "sum"), 3.0);
}

BONK_TEST(js_lexer, keywords_and_identifiers_are_distinct) {
  const std::string source = "var iffy = 5; var $dollar = 6; var _under = 7;";
  CHECK_EQ(number_of_var(source, "iffy"), 5.0);
  CHECK_EQ(number_of_var(source, "$dollar"), 6.0);
  CHECK_EQ(number_of_var(source, "_under"), 7.0);
}
