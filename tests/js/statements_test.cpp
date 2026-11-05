// Statement-level features from stage 32: the `with` statement (an object
// environment in front of the scope chain), `switch` with fallthrough and
// `default`, and labelled statements (`break label` / `continue label`).
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

// ------------------------------------------------------------------- with ----

BONK_TEST(js_statements, with_reads_and_writes_through_the_object) {
  CHECK_EQ(num("var o = {a: 1}; with (o) { a = 5; } var v = o.a;"), 5.0);
  CHECK_EQ(num("var o = {a: 2}; var a = 1; with (o) { var v = a; }"), 2.0);
}

BONK_TEST(js_statements, with_falls_back_to_the_outer_scope) {
  CHECK_EQ(num("var a = 7; with ({}) { var v = a; }"), 7.0);
  // A missing property is *not* created on the object: the write creates a global.
  CHECK_EQ(str("var o = {}; with (o) { fresh = 3; } var v = String(o.fresh);"), std::string("undefined"));
  CHECK_EQ(num("var o = {}; with (o) { fresh = 3; } var v = fresh;"), 3.0);
}

BONK_TEST(js_statements, with_sees_inherited_properties) {
  CHECK_EQ(str("function M() {} M.prototype.m = function () { return 'inherited'; };"
               "var o = new M(); with (o) { var v = m(); }"),
           std::string("inherited"));
  CHECK_EQ(num("var o = {hasOwnProperty: 1}; with (o) { var v = hasOwnProperty; }"), 1.0);
}

BONK_TEST(js_statements, with_boxes_string_primitives) {
  CHECK_EQ(num("with ('abc') { var v = length; }"), 3.0);
  // Other primitives box to an empty object: the body still runs, nothing resolves.
  CHECK_EQ(num("var v = 0; with (5) { v = 1; }"), 1.0);
}

BONK_TEST(js_statements, with_delete_removes_the_object_property) {
  CHECK_EQ(str("var o = {p: 1}; with (o) { delete p; } var v = String(o.p);"), std::string("undefined"));
  // Deleting a real binding reports false and leaves it alone.
  CHECK_EQ(str("var a = 1; var v = String(delete a) + a;"), std::string("false1"));
}

BONK_TEST(js_statements, with_is_rejected_in_strict_code) {
  CHECK(fails_with("'use strict'; with ({}) {}", "strict mode"));
}

BONK_TEST(js_statements, with_throws_on_nullish) {
  CHECK(fails_with("with (null) {}", "TypeError"));
}

// ----------------------------------------------------------------- switch ----

BONK_TEST(js_statements, switch_matches_with_strict_equality) {
  CHECK_EQ(str("function f(v) { switch (v) { case 1: return 'num'; case '1': return 'str';"
               " default: return 'other'; } } var v = f(1) + f('1') + f(true);"),
           std::string("numstrother"));
  CHECK_EQ(str("function f(v) { switch (v) { case undefined: return 'u'; } return 'x'; } var v = f();"),
           std::string("u"));
}

BONK_TEST(js_statements, switch_falls_through_and_honours_break) {
  CHECK_EQ(str("function f(v) { var out = ''; switch (v) { case 1: out += 'a'; case 2: out += 'b';"
               " break; case 3: out += 'c'; } return out || 'none'; }"
               "var v = f(1) + '|' + f(2) + '|' + f(3) + '|' + f(4);"),
           std::string("ab|b|c|none"));
}

BONK_TEST(js_statements, switch_default_runs_only_when_nothing_matches) {
  CHECK_EQ(str("function f(v) { var out = ''; switch (v) { default: out += 'd'; case 9: out += '9'; break;"
               " case 1: out += '1'; } return out; } var v = f(9) + '|' + f(1) + '|' + f(0);"),
           std::string("9|1|d9"));
}

BONK_TEST(js_statements, switch_evaluates_the_discriminant_and_tests_in_order) {
  CHECK_EQ(num("var calls = 0; function t(v) { calls++; return v; }"
               "switch (t(1)) { case t(0): break; case t(1): break; case t(2): break; } var v = calls;"),
           3.0);  // discriminant + two clause tests: the third clause is never evaluated
}

BONK_TEST(js_statements, switch_inside_a_loop_and_inside_a_function) {
  CHECK_EQ(str("var out = ''; for (var i = 0; i < 3; i++) { switch (i) { case 0: out += 'z'; break;"
               " default: out += i; } out += '.'; } var v = out;"),
           std::string("z.1.2."));
  CHECK_EQ(str("function f(v) { switch (v) { case 1: return 'one'; default: return 'other'; } }"
               "var v = f(1) + f(2);"),
           std::string("oneother"));
}

BONK_TEST(js_statements, switch_allows_a_single_default_only) {
  CHECK(fails_with("switch (1) { default: break; default: break; }", "default"));
}

BONK_TEST(js_statements, switch_break_stays_inside_the_switch) {
  CHECK_EQ(str("var out = ''; for (var i = 0; i < 2; i++) { switch (1) { case 1: break; } out += i; }"
               "var v = out;"),
           std::string("01"));
}

// ------------------------------------------------------------------ labels ---

BONK_TEST(js_statements, break_label_leaves_the_labelled_statement) {
  CHECK_EQ(num("var v = 0; block: { v = 1; break block; v = 2; }"), 1.0);
  CHECK_EQ(num("var n = 0; outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) {"
               " n++; if (j === 1) break outer; } } var v = n;"),
           2.0);
}

BONK_TEST(js_statements, continue_label_targets_the_outer_loop) {
  CHECK_EQ(str("var out = ''; outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) {"
               " if (j === 1) continue outer; out += i + ':' + j + ' '; } } var v = out;"),
           std::string("0:0 1:0 2:0 "));
}

BONK_TEST(js_statements, labels_nest_and_do_not_leak) {
  CHECK_EQ(str("var out = ''; a: for (var i = 0; i < 2; i++) { b: for (var j = 0; j < 2; j++) {"
               " if (j === 1) continue a; out += i + '' + j; } } var v = out;"),
           std::string("0010"));
  // The same label name may be reused by a sibling statement.
  CHECK_EQ(num("var v = 0; x: { v = 1; break x; } x: { v = v + 1; }"), 2.0);
}

BONK_TEST(js_statements, break_label_works_from_inside_a_switch) {
  CHECK_EQ(str("var out = ''; loop: for (var i = 0; i < 3; i++) { switch (i) {"
               " case 1: break loop; default: out += i; } } var v = out;"),
           std::string("0"));
}

// -------------------------------------------------- the global object -------

BONK_TEST(js_statements, global_properties_and_variables_are_the_same_thing) {
  CHECK_EQ(num("this.p1 = 1; var v = p1;"), 1.0);
  CHECK_EQ(num("p2 = 2; var v = this.p2 + globalThis.p2;"), 4.0);
  CHECK_EQ(num("var v = ('p3' in globalThis) ? 1 : 0;"), 0.0);
  CHECK_EQ(num("this.p3 = 3; var v = ('p3' in globalThis) ? 1 : 0;"), 1.0);
}

BONK_TEST(js_statements, this_inside_blocks_and_with_is_the_caller) {
  CHECK_EQ(num("var v = 0; { v = 1; }"), 1.0);
  CHECK_EQ(str("var o = { get t() { return this === o ? 'recv' : 'lost'; } };"
               "function f() { with (o) { return t; } } var v = f.call(o);"),
           std::string("recv"));
  CHECK_EQ(num("var o = {}; with (o) { var v = this === globalThis ? 1 : 0; }"), 1.0);
}

BONK_TEST(js_statements, isfinite_is_a_global) {
  CHECK_EQ(num("var v = isFinite(1) + isFinite('2') + isFinite(Infinity) + isFinite(NaN);"), 2.0);
  CHECK_EQ(num("function f() { return isFinite; } var v = typeof f() === 'function' ? 1 : 0;"), 1.0);
}

// ------------------------------------------------------- early errors -------

BONK_TEST(js_statements, const_requires_an_initializer) {
  CHECK(fails_with("const a;", "missing initializer"));
  CHECK(fails_with("const {a};", "missing initializer"));
  CHECK(fails_with("const [a];", "missing initializer"));
  CHECK_EQ(num("const a = 4; var v = a;"), 4.0);
  CHECK_EQ(num("for (const k in {x: 1}) {} var v = 1;"), 1.0);
}

BONK_TEST(js_statements, switch_rejects_duplicate_lexical_declarations) {
  CHECK(fails_with("switch (0) { case 1: let a; break; case 2: let a; }", "duplicate lexical"));
  CHECK(fails_with("switch (0) { case 1: class f {} default: let f }", "duplicate lexical"));
  CHECK(fails_with("switch (0) { case 1: var a; break; case 2: let a; }", "same switch"));
  CHECK_EQ(num("switch (0) { case 1: let a = 1; break; case 2: let b = 2; } var v = 1;"), 1.0);
}

BONK_TEST(js_statements, declarations_cannot_be_labelled) {
  CHECK(fails_with("a: let x = 1;", "cannot be labelled"));
  CHECK(fails_with("a: const x = 1;", "cannot be labelled"));
  CHECK(fails_with("a: class C {}", "cannot be labelled"));
  CHECK(fails_with("'use strict'; a: function f() {}", "cannot be labelled"));
  CHECK_EQ(num("var v = 1; a: function f() {}"), 1.0);  // sloppy: a labelled function is legal
  CHECK_EQ(num("var v = 0; a: { v = 2; break a; }"), 2.0);
}

// ------------------------------------------------ unicode escapes -----------

BONK_TEST(js_statements, identifiers_may_use_unicode_escapes) {
  CHECK_EQ(num("var \\u0061 = 5; var v = a;"), 5.0);
  CHECK_EQ(num("var x = {\\u0062: 2}; var v = x.b;"), 2.0);
  CHECK_EQ(num("var \\u{63} = 3; var v = c;"), 3.0);
  CHECK_EQ(num("var \\u0061wait = 7; var v = await;"), 7.0);
  // The escape is decoded before the strict-mode checks run, so this is still `eval`.
  CHECK(fails_with("'use strict'; var \\u0065val = 1;", "strict mode"));
}
