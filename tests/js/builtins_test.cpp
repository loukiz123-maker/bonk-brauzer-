// Builtins: console capture, Math, JSON, parseInt/parseFloat, array/string
// methods, timers and host callbacks.
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

BONK_TEST(js_builtins, console_collects_entries) {
  Context context;
  const ExecutionResult result = context.evaluate(
      "console.log('hello', 1, true);\n"
      "console.warn('careful');\n"
      "console.error('bad');\n");
  CHECK(result.ok);
  REQUIRE(context.console().size() == 3);
  CHECK_EQ(context.console()[0].message, std::string("hello 1 true"));
  CHECK_EQ(context.console()[1].message, std::string("careful"));
  CHECK_EQ(static_cast<int>(context.console()[1].level), static_cast<int>(ConsoleEntry::Level::Warn));
  CHECK_EQ(static_cast<int>(context.console()[2].level), static_cast<int>(ConsoleEntry::Level::Error));
  context.clear_console();
  CHECK_EQ(context.console().size(), std::size_t(0));
}

BONK_TEST(js_builtins, math_functions) {
  CHECK_EQ(num("var v = Math.max(1, 5, 3);"), 5.0);
  CHECK_EQ(num("var v = Math.min(1, 5, 3);"), 1.0);
  CHECK_EQ(num("var v = Math.round(2.6);"), 3.0);
  CHECK_EQ(num("var v = Math.floor(-1.2);"), -2.0);
  CHECK_EQ(num("var v = Math.ceil(1.2);"), 2.0);
  CHECK_EQ(num("var v = Math.abs(-7);"), 7.0);
  CHECK_EQ(num("var v = Math.pow(2, 10);"), 1024.0);
  CHECK_EQ(num("var v = Math.sqrt(81);"), 9.0);
}

BONK_TEST(js_builtins, json_round_trip) {
  CHECK_EQ(str("var v = JSON.stringify({a: 1, b: [true, null, 'x']});"),
           std::string("{\"a\":1,\"b\":[true,null,\"x\"]}"));
  CHECK_EQ(num("var o = JSON.parse('{\"n\": 7}'); var v = o.n;"), 7.0);
  CHECK_EQ(str("var a = JSON.parse('[1, 2, 3]'); var v = a.join('-');"), std::string("1-2-3"));
}

BONK_TEST(js_builtins, parse_int_and_float) {
  CHECK_EQ(num("var v = parseInt('42px');"), 42.0);
  CHECK_EQ(num("var v = parseInt('ff', 16);"), 255.0);
  CHECK_EQ(num("var v = parseFloat('1.5em');"), 1.5);
  CHECK_EQ(num("var v = isNaN('abc') ? 1 : 0;"), 1.0);
}

BONK_TEST(js_builtins, array_methods) {
  CHECK_EQ(num("var a = [3, 1, 2]; a.push(4); var v = a.length;"), 4.0);
  CHECK_EQ(num("var a = [1, 2]; var v = a.pop();"), 2.0);
  CHECK_EQ(num("var a = [1, 2, 3]; var v = a.indexOf(2);"), 1.0);
  CHECK_EQ(str("var a = [1, 2, 3]; var v = a.join('+');"), std::string("1+2+3"));
  CHECK_EQ(num("var a = [1, 2, 3, 4]; var v = a.filter(function(x) { return x % 2 === 0; }).length;"), 2.0);
  CHECK_EQ(num("var a = [1, 2, 3]; var v = a.map(function(x) { return x * x; })[2];"), 9.0);
  CHECK_EQ(num("var a = [1, 2, 3]; var s = 0; a.forEach(function(x) { s += x; }); var v = s;"), 6.0);
  CHECK_EQ(num("var a = [1, 2, 3]; var v = a.slice(1).length;"), 2.0);
  CHECK_EQ(str("var a = [1, 2, 3]; var v = a.reverse().join('');"), std::string("321"));
  CHECK_EQ(num("var a = [5, 6]; var v = a.find(function(x) { return x === 6; });"), 6.0);
}

BONK_TEST(js_builtins, string_methods) {
  CHECK_EQ(str("var v = 'Hello'.toUpperCase();"), std::string("HELLO"));
  CHECK_EQ(str("var v = 'Hello'.toLowerCase();"), std::string("hello"));
  CHECK_EQ(str("var v = '  x  '.trim();"), std::string("x"));
  CHECK_EQ(num("var v = 'banana'.indexOf('na');"), 2.0);
  CHECK_EQ(num("var v = 'banana'.includes('nan') ? 1 : 0;"), 1.0);
  CHECK_EQ(str("var v = 'a,b,c'.split(',').join('|');"), std::string("a|b|c"));
  CHECK_EQ(str("var v = 'a-b'.replace('-', '+');"), std::string("a+b"));
  CHECK_EQ(str("var v = 'abc'.slice(1);"), std::string("bc"));
  CHECK_EQ(str("var v = 'abc'.charAt(2);"), std::string("c"));
  CHECK_EQ(num("var v = 'abc'.length;"), 3.0);
}

BONK_TEST(js_builtins, number_formatting) {
  CHECK_EQ(str("var v = (1.005).toFixed(2);"), std::string("1.00"));
  CHECK_EQ(str("var v = String(42);"), std::string("42"));
  CHECK_EQ(num("var v = Number('42') + 1;"), 43.0);
  CHECK_EQ(str("var v = String(0.5);"), std::string("0.5"));
}

BONK_TEST(js_builtins, error_construction) {
  CHECK_EQ(str("var e = new Error('kaput'); var v = e.message;"), std::string("kaput"));
  CHECK_EQ(str("var v = typeof new Error('x').name;"), std::string("string"));
}

BONK_TEST(js_builtins, timers_run_when_pumped) {
  Context context;
  const ExecutionResult result = context.evaluate(
      "var ticks = 0;\n"
      "setTimeout(function() { ticks++; });\n"
      "setTimeout(function() { ticks++; });\n");
  CHECK(result.ok);
  CHECK_EQ(to_number(context.runtime().global("ticks")), 0.0);
  context.run_pending_callbacks();
  CHECK_EQ(to_number(context.runtime().global("ticks")), 2.0);
}

BONK_TEST(js_builtins, host_globals_can_be_injected) {
  Context context;
  auto document = std::make_shared<Object>();
  document->properties["title"] = Property{Value::string_of("Bonk")};
  context.set_global("document", Value::object_of(document));
  const ExecutionResult result = context.evaluate("var title = document.title;");
  CHECK(result.ok);
  CHECK_EQ(to_string(context.runtime().global("title")), std::string("Bonk"));
}

BONK_TEST(js_builtins, function_call_and_apply) {
  CHECK_EQ(num(
               "function add(a, b) { return this.base + a + b; }\n"
               "var v = add.call({base: 10}, 1, 2);"),
           13.0);
  CHECK_EQ(num(
               "function add(a, b) { return a + b; }\n"
               "var v = add.apply(null, [4, 5]);"),
           9.0);
}

BONK_TEST(js_builtins, console_then_exception_reports_message) {
  Context context;
  const ExecutionResult result = context.evaluate("throw new TypeError('bad input');");
  CHECK(!result.ok);
  CHECK_EQ(result.message.find("bad input") != std::string::npos, true);
}
