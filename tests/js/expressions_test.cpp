// Expression-level syntax from ES2015-ES2020: numeric separators, logical
// assignment, optional chaining (stage 29).
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

BONK_TEST(js_expressions, numeric_separators_and_radix_prefixes) {
  CHECK_EQ(num("var v = 1_000_000;"), 1000000.0);
  CHECK_EQ(num("var v = 0xFF_FF;"), 65535.0);
  CHECK_EQ(num("var v = 0b1010_1010;"), 170.0);
  CHECK_EQ(num("var v = 0o17;"), 15.0);
  CHECK_EQ(num("var v = 1_0.5_5;"), 10.55);
}

BONK_TEST(js_expressions, logical_assignment_short_circuits) {
  CHECK_EQ(num("var a = 0; a ||= 5; var v = a;"), 5.0);
  CHECK_EQ(num("var a = 1; a ||= 9; var v = a;"), 1.0);
  CHECK_EQ(num("var a = 1; a &&= 4; var v = a;"), 4.0);
  CHECK_EQ(num("var a = 0; a &&= 4; var v = a;"), 0.0);
  CHECK_EQ(str("var a = null; a ?" "?= 3; var v = String(a);"), std::string("3"));
  CHECK_EQ(num("var a = 0; a ?" "?= 3; var v = a;"), 0.0);
}

BONK_TEST(js_expressions, logical_assignment_evaluates_the_reference_once) {
  const std::string source =
      "var gets = 0; var sets = 0;"
      "var o = {get p() { gets++; return 0; }, set p(v) { sets++; }};"
      "o.p ||= 7; var v = gets * 10 + sets;";
  CHECK_EQ(num(source), 11.0);
  CHECK_EQ(num("var calls = 0; function k() { calls++; return 'z'; }"
               "var o = {z: 5}; o[k()] ?" "?= 9; var v = calls * 100 + o.z;"),
           105.0);
}

BONK_TEST(js_expressions, optional_chaining_returns_undefined_instead_of_throwing) {
  CHECK_EQ(str("var a = null; var v = String(a?.b);"), std::string("undefined"));
  CHECK_EQ(str("var a; var v = String(a?.b.c);"), std::string("undefined"));
  CHECK_EQ(str("var a = null; var v = String(a?.[0]);"), std::string("undefined"));
  CHECK_EQ(str("var f = null; var v = String(f?.());"), std::string("undefined"));
  CHECK_EQ(num("var o = {b: {c: 1}}; var v = o?.b?.c;"), 1.0);
  CHECK_EQ(num("var o = {b: {c: 1}}; var v = o.b.c;"), 1.0);
}

BONK_TEST(js_expressions, optional_chaining_keeps_the_receiver_and_skips_side_effects) {
  CHECK_EQ(str("var o = {m: function () { return this === o ? 'recv' : 'lost'; }};"
               "var v = o?.m();"),
           std::string("recv"));
  CHECK_EQ(num("var n = 0; function side() { n++; return 'k'; } var o = null; o?.[side()]; var v = n;"),
           0.0);
  CHECK_EQ(num("var n = 0; function side() { n++; return 1; } var o = {a: [7, 8]};"
               "var v = o?.a?.[side()] + n * 10;"),
           18.0);
}

BONK_TEST(js_expressions, optional_chain_is_not_an_assignment_target) {
  CHECK(fails_with("var o = {}; o?.a = 1;", "assignment"));
  CHECK_EQ(num("var o = {}; var v = o?.missing ?? 5;"), 5.0);
}
