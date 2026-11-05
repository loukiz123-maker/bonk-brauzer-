// ES2015 classes: constructors, prototype/static methods, accessors, extends/super (stage 28).
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

BONK_TEST(js_classes, constructor_and_methods) {
  const std::string source =
      "class Point { constructor(x, y) { this.x = x; this.y = y; }"
      " sum() { return this.x + this.y; } }"
      "var p = new Point(3, 4); var v = p.sum();";
  CHECK_EQ(num(source), 7.0);
  CHECK_EQ(str("class P { constructor(a) { this.a = a; } } var v = new P(5).a;"), std::string("5"));
}

BONK_TEST(js_classes, instances_share_a_non_enumerable_prototype) {
  CHECK_EQ(num("class A { m() {} } var v = Object.keys(A.prototype).length;"), 0.0);
  CHECK_EQ(num("class A { m() {} } var o = new A(); var v = o.m === A.prototype.m;"), 1.0);
  CHECK_EQ(num("class A {} var o = new A(); var v = o instanceof A;"), 1.0);
  CHECK_EQ(num("class A {} A.prototype.tag = 7; var v = new A().tag;"), 7.0);
}

BONK_TEST(js_classes, static_methods_and_name) {
  CHECK_EQ(num("class A { static make() { return 42; } } var v = A.make();"), 42.0);
  CHECK_EQ(str("class Widget {} var v = Widget.name;"), std::string("Widget"));
  CHECK_EQ(num("class A { static m() {} } var v = Object.keys(A).length;"), 0.0);
}

BONK_TEST(js_classes, accessors_receive_the_instance) {
  const std::string source =
      "class Box { constructor(v) { this._v = v; } get v() { return this._v; }"
      " set v(next) { this._v = next * 2; } }"
      "var b = new Box(1); b.v = 5; var v = b.v;";
  CHECK_EQ(num(source), 10.0);
}

BONK_TEST(js_classes, extends_inherits_methods_and_sets_the_chain) {
  const std::string source =
      "class Animal { constructor(name) { this.name = name; } speak() { return this.name + '!'; } }"
      "class Dog extends Animal { constructor(name) { super(name); } }"
      "var d = new Dog('Rex'); var v = d.speak() + ',' + (d instanceof Animal) + ',' + (d instanceof Dog);";
  CHECK_EQ(str(source), std::string("Rex!,true,true"));
}

BONK_TEST(js_classes, super_calls_the_parent_method_with_the_same_this) {
  const std::string source =
      "class A { m() { return 'a' + this.tag; } }"
      "class B extends A { constructor() { super(); this.tag = 1; } m() { return super.m() + 'b'; } }"
      "var v = new B().m();";
  CHECK_EQ(str(source), std::string("a1b"));
}

BONK_TEST(js_classes, derived_default_constructor_forwards_arguments) {
  const std::string source =
      "class A { constructor(x) { this.x = x; } }"
      "class B extends A {}"
      "var v = new B(9).x;";
  CHECK_EQ(num(source), 9.0);
}

BONK_TEST(js_classes, class_expressions_work_anonymously_and_named) {
  CHECK_EQ(str("var C = class { greet() { return 'hi'; } }; var v = new C().greet();"), std::string("hi"));
  CHECK_EQ(str("var C = class Inner {}; var v = C.name;"), std::string("Inner"));
}

BONK_TEST(js_classes, extends_a_non_constructor_is_a_type_error) {
  CHECK(fails_with("class A extends 5 {}", "not a constructor"));
}

BONK_TEST(js_classes, subclasses_of_builtins_work) {
  CHECK_EQ(num("class MyError extends Error {} var e = new MyError('boom'); var v = e.message.length;"), 4.0);
  CHECK_EQ(str("class MyArr extends Array {} var a = new MyArr(); a.push(1, 2); var v = a.join('-');"), std::string("1-2"));
}
