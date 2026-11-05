// Strict mode (stage 31): the "use strict" directive, the early errors it enables and
// the runtime semantics that differ between strict and sloppy code. Also covers the
// parser fixes that came with it: numeric literals followed by a member access, method
// shorthand in object literals, computed property keys and accessor arity checks.
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

// ---------------------------------------------------------------- directives --

BONK_TEST(js_strict, program_directive_makes_undeclared_writes_throw) {
  CHECK(fails_with("'use strict'; missing = 1;", "ReferenceError"));
  // The same statement is legal sloppy code and creates a global.
  CHECK_EQ(num("missing = 2; var v = missing;"), 2.0);
}

BONK_TEST(js_strict, directive_only_counts_in_a_directive_prologue) {
  // A string expression after a real statement is not a directive.
  CHECK_EQ(num("var v = 1; 'use strict'; v = 2; var w = v;", "w"), 2.0);
  // ... and an unrelated leading string does not switch the mode on by itself.
  CHECK_EQ(num("'not strict'; v = 3;"), 3.0);
}

BONK_TEST(js_strict, function_directive_is_local_to_the_function) {
  CHECK(fails_with("function f() { 'use strict'; inner = 1; } f();", "ReferenceError"));
  CHECK_EQ(num("outer = 4; var v = outer;"), 4.0);
}

BONK_TEST(js_strict, nested_functions_inherit_strictness) {
  CHECK(fails_with("'use strict'; function outer() { function inner() { deep = 1; } inner(); } outer();",
                   "ReferenceError"));
  CHECK(fails_with("function outer() { 'use strict'; return function () { deep = 1; }; } outer()();",
                   "ReferenceError"));
}

BONK_TEST(js_strict, strictness_does_not_leak_out_of_a_function_body) {
  // The enclosing script stays sloppy even though the function body is strict.
  CHECK_EQ(num("function f() { 'use strict'; return 1; } loose = 5; var v = f() + loose;"), 6.0);
}

BONK_TEST(js_strict, this_is_undefined_in_strict_callees) {
  CHECK_EQ(str("'use strict'; function f() { return String(this); } var v = f();"), std::string("undefined"));
  CHECK_EQ(str("function f() { return typeof this; } var v = f();"), std::string("object"));
  CHECK_EQ(str("'use strict'; var v = (function () { return String(this); })();"), std::string("undefined"));
}

BONK_TEST(js_strict, sloppy_callees_get_the_global_object_as_this) {
  CHECK_EQ(str("function f() { return typeof this; } var v = f();"), std::string("object"));
  CHECK_EQ(num("var global = this; function c() { return this === global ? 1 : 0; } var v = c();"), 1.0);
  CHECK_EQ(num("function f() { return this === globalThis ? 1 : 0; } var v = f();"), 1.0);
  CHECK_EQ(num("function f() { return this === globalThis ? 1 : 0; } var v = f.call(null);"), 1.0);
  // Avoid `this.x` on the global object: this engine keeps globals in a scope, not
  // as properties of `globalThis` (documented limitation).
  CHECK_EQ(str("'use strict'; function f() { return typeof this; } var v = f.call(null);"),
           std::string("object"));
}

BONK_TEST(js_strict, top_level_this_is_the_global_object) {
  CHECK_EQ(num("var v = this === globalThis ? 1 : 0;"), 1.0);
  CHECK_EQ(num("function f() { return this; } var v = f() === this ? 1 : 0;"), 1.0);
}

// -------------------------------------------------------------- early errors --

BONK_TEST(js_strict, with_statement_is_a_syntax_error) {
  CHECK(fails_with("'use strict'; with (obj) {}", "strict mode"));
  CHECK(fails_with("function f() { 'use strict'; with (obj) {} }", "strict mode"));
}

BONK_TEST(js_strict, deleting_an_identifier_is_a_syntax_error) {
  CHECK(fails_with("'use strict'; var a = 1; delete a;", "strict mode"));
  CHECK_EQ(num("var a = 1; delete a; var v = a;"), 1.0);  // sloppy: a no-op that returns false
}

BONK_TEST(js_strict, octal_literals_and_escapes_are_syntax_errors) {
  CHECK(fails_with("'use strict'; var a = 010;", "octal"));
  CHECK(fails_with("'use strict'; var a = '\\1';", "octal escape"));
  CHECK_EQ(num("var a = 010; var v = a;"), 8.0);
  CHECK_EQ(num("var v = 017;"), 15.0);
}

BONK_TEST(js_strict, eval_and_arguments_are_not_bindable_names) {
  CHECK(fails_with("'use strict'; var eval = 1;", "strict mode"));
  CHECK(fails_with("'use strict'; var arguments = 1;", "strict mode"));
  CHECK(fails_with("'use strict'; function f(eval) {}", "strict mode"));
  CHECK(fails_with("'use strict'; function eval() {}", "strict mode"));
  CHECK(fails_with("'use strict'; function arguments() {}", "strict mode"));
  CHECK(fails_with("'use strict'; try {} catch (eval) {}", "strict mode"));
}

BONK_TEST(js_strict, eval_and_arguments_are_not_assignable) {
  CHECK(fails_with("'use strict'; eval = 1;", "strict mode"));
  CHECK(fails_with("'use strict'; arguments = 1;", "strict mode"));
  CHECK(fails_with("'use strict'; eval += 1;", "strict mode"));
  CHECK(fails_with("'use strict'; eval++;", "strict mode"));
  CHECK(fails_with("'use strict'; --arguments;", "strict mode"));
}

BONK_TEST(js_strict, reserved_words_are_only_reserved_in_strict_code) {
  CHECK(fails_with("'use strict'; var let = 1;", "js.syntax"));
  CHECK(fails_with("'use strict'; var static = 1;", "js.syntax"));
  CHECK_EQ(num("var let = 1; var static = 2; var yield = 3; var v = let + static + yield;"), 6.0);
  CHECK_EQ(num("function f(static, yield) { return static * yield; } var v = f(2, 5);"), 10.0);
  CHECK_EQ(num("var v = (function (let) { return let; })(9);"), 9.0);
}

BONK_TEST(js_strict, duplicate_parameters) {
  CHECK(fails_with("'use strict'; function f(a, a) {}", "already declared"));
  CHECK_EQ(num("function f(a, a) { return a; } var v = f(1, 2);"), 2.0);  // sloppy allows it
  // A non-simple parameter list is strict about duplicates in either mode (ES2015 14.1.2).
  CHECK(fails_with("function f(a, a = 1) {}", "already declared"));
  CHECK(fails_with("function f([a], [a]) {}", "already declared"));
  CHECK(fails_with("function f(a, ...a) {}", "already declared"));
  CHECK(fails_with("function f({a}, a) {}", "already declared"));
}

BONK_TEST(js_strict, class_bodies_are_always_strict) {
  CHECK(fails_with("class A { m() { with ({}) {} } }", "strict mode"));
  CHECK(fails_with("class A { m(a, a) {} }", "already declared"));
  CHECK(fails_with("class A { m() { var eval = 1; } }", "strict mode"));
  CHECK(fails_with("class A { m() { delete x; } }", "strict mode"));
  CHECK_EQ(str("class A { m() { return String(this); } } var a = new A();"
                "var f = a.m; var v = f();"),
            "undefined");
}

// -------------------------------------------------------- runtime semantics --

BONK_TEST(js_strict, writes_to_read_only_properties_throw) {
  const std::string strict = "'use strict'; var o = {};"
                             "Object.defineProperty(o, 'p', {value: 1, writable: false});"
                             "o.p = 2;";
  CHECK(fails_with(strict, "TypeError"));
  // Sloppy code fails silently and leaves the value alone.
  CHECK_EQ(num("var o = {}; Object.defineProperty(o, 'p', {value: 1, writable: false});"
               "o.p = 2; var v = o.p;"),
           1.0);
}

BONK_TEST(js_strict, writes_to_non_extensible_objects_throw) {
  const std::string strict = "'use strict'; var o = {}; Object.preventExtensions(o); o.fresh = 1;";
  CHECK(fails_with(strict, "TypeError"));
  CHECK_EQ(str("var o = {}; Object.preventExtensions(o); o.fresh = 1; var v = String(o.fresh);"),
           std::string("undefined"));
}

BONK_TEST(js_strict, strictness_follows_the_function_into_nested_scopes) {
  CHECK(fails_with("function outer() { 'use strict'; var o = {};"
                   "Object.defineProperty(o, 'p', {value: 1, writable: false});"
                   "for (var i = 0; i < 1; i++) { o.p = 2; } } outer();",
                   "TypeError"));
}

BONK_TEST(js_strict, arguments_is_a_plain_object_in_strict_functions) {
  // Function#arguments is a plain object here; strict callers still see the value.
  CHECK_EQ(num("'use strict'; function f(a) { a = 2; return arguments[0]; } var v = f(1);"), 1.0);
  // ... while sloppy functions keep the mapped object (10.4.4.7 vs 10.4.4.8).
  CHECK_EQ(num("function f(a) { a = 2; return arguments[0]; } var v = f(1);"), 2.0);
}

// ------------------------------------------- parser fixes that came with it --

BONK_TEST(js_strict, numeric_literals_can_be_followed_by_a_member_access) {
  CHECK_EQ(str("var v = 1.5.toString();"), "1.5");
  CHECK_EQ(str("var v = 0..toString();"), "0");
  CHECK_EQ(str("var v = 1e3.toString();"), "1000");
  CHECK_EQ(str("var v = 1.5.toFixed(2);"), "1.50");
  // A numeric literal may not run straight into an identifier: `1.toString` is `1.` + `toString`.
  CHECK(fails_with("1.toString();", "js.syntax"));
  CHECK(fails_with("var x = 1.toString;", "js.syntax"));
}

BONK_TEST(js_strict, object_literal_method_shorthand) {
  CHECK_EQ(num("var o = { m() { return 1; } }; var v = o.m();"), 1.0);
  CHECK_EQ(num("var o = { m() { return this.k; }, k: 4 }; var v = o.m();"), 4.0);
  CHECK_EQ(num("var o = { 'n'() { return 2; }, 3() { return 3; } }; var v = o.n() + o[3]();"), 5.0);
  CHECK_EQ(num("var o = { m(a, b) { return a + b; } }; var v = o.m(2, 3);"), 5.0);
}

BONK_TEST(js_strict, computed_property_keys_in_object_literals) {
  CHECK_EQ(num("var k = 'a'; var o = { [k]: 1 }; var v = o.a;"), 1.0);
  CHECK_EQ(num("var o = { ['b' + 2]: 3 }; var v = o.b2;"), 3.0);
  CHECK_EQ(num("var o = { [1 + 1]() { return 4; } }; var v = o[2]();"), 4.0);
  CHECK_EQ(num("var o = { get ['x']() { return 5; } }; var v = o.x;"), 5.0);
  // The key is evaluated before the value.
  CHECK_EQ(str("var log = []; var o = { [log.push('k'), 'a']: log.push('v') }; var v = log.join('');"), "kv");
}

BONK_TEST(js_strict, computed_property_keys_in_classes) {
  CHECK_EQ(num("var n = 'dyn'; class A { [n]() { return 7; } } var v = new A().dyn();"), 7.0);
  CHECK_EQ(num("class A { static ['s']() { return 8; } } var v = A.s();"), 8.0);
  CHECK_EQ(num("class A { get ['g']() { return 9; } } var v = new A().g;"), 9.0);
  // A computed key never defines the constructor.
  CHECK_EQ(str("class A { ['constructor']() { return 'm'; } } var v = typeof new A().constructor;"),
           std::string("function"));
}

BONK_TEST(js_strict, computed_property_keys_in_patterns) {
  CHECK_EQ(num("var k = 'a'; var { [k]: v } = { a: 5 };"), 5.0);
  CHECK_EQ(num("function f({ [1]: q }) { return q; } var v = f({ 1: 8 });"), 8.0);
  CHECK_EQ(num("var a = 1, b = 2; var { [a + b]: s, ...rest } = { 3: 9, x: 1 };"
               "var v = s + Object.keys(rest).length;"),
           10.0);
}

BONK_TEST(js_strict, accessor_arity_is_checked) {
  CHECK(fails_with("var o = { get a(x) {} };", "getter must not have parameters"));
  CHECK(fails_with("var o = { set a() {} };", "setter must have exactly one parameter"));
  CHECK(fails_with("var o = { set a(x, y) {} };", "setter must have exactly one parameter"));
  CHECK(fails_with("class A { get a(x) {} }", "getter must not have parameters"));
  CHECK(fails_with("class A { set a() {} }", "setter must have exactly one parameter"));
  CHECK_EQ(num("var o = { get a() { return 1; }, set a(v) { this.b = v; } }; o.a = 3; var v = o.b;"), 3.0);
  CHECK_EQ(num("var o = { get 0() { return 6; } }; var v = o[0];"), 6.0);
}

BONK_TEST(js_strict, new_expressions_nest) {
  // `new new Boolean(true)` parses, then fails at run time: the inner `new` already
  // produced an object, which is not a constructor.
  CHECK(fails_with("new new Boolean(true);", "TypeError"));
  CHECK_EQ(num("function C() { this.x = 3; } function D() { this.c = C; }"
               "var v = new (new D().c)().x;"),
           3.0);
}

BONK_TEST(js_strict, catch_clause_patterns) {
  CHECK_EQ(num("try { throw { message: 'boom', code: 42 }; } catch ({ code }) { var v = code; }"), 42.0);
  CHECK_EQ(str("try { throw { message: 'boom' }; } catch ({ message }) { var v = message; }"), std::string("boom"));
  CHECK_EQ(num("function f() { try { throw [1, 2]; } catch ([a, b]) { return a + b; } } var v = f();"), 3.0);
  CHECK_EQ(num("try { throw 1; } catch (e) { var v = e; }"), 1.0);
}
