// Internal AST for the Bonk Brauzer JavaScript engine (not part of the public API).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace bonk::js::ast {

struct Expr;
struct Stmt;
struct FunctionNode;
struct ClassNode;

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
using FunctionPtr = std::shared_ptr<FunctionNode>;

enum class ExprKind {
  Number,
  String,
  Boolean,
  Null,
  Undefined,
  Identifier,
  This,
  Array,
  ObjectLiteral,
  Function,
  Unary,
  Update,
  Binary,
  Logical,
  Assign,
  Conditional,
  Call,
  New,
  Member,
  Sequence,
  Template,
  Class,
  Spread,  // `...expr` in an array literal, a call's arguments or a pattern
};

/// One method of a class body: `m() {}`, `static m() {}`, `get m() {}`, `set m(v) {}`.
struct ClassMethod {
  std::string key;
  /// `class A { [expr]() {} }` — when set, the key is computed at run time.
  ExprPtr computed_key;
  FunctionPtr function;
  bool is_static = false;
  bool is_get = false;
  bool is_set = false;
};

/// `class Name extends Super { … }` — shared by declarations and expressions.
struct ClassNode {
  std::string name;          // empty for anonymous class expressions
  ExprPtr super_class;       // null when there is no `extends`
  std::vector<ClassMethod> methods;
};

struct PropertyDef {
  /// `{a: 1}` is data; `{get a() {…}}` / `{set a(v) {…}}` are accessors;
  /// `{...o}` copies own enumerable properties.
  enum class Kind { Data, Get, Set, Spread } kind = Kind::Data;
  std::string key;
  /// `{[expr]: value}` — when set, the key is computed at run time (and `key` is empty).
  ExprPtr computed_key;
  ExprPtr value;
};

struct Expr {
  ExprKind kind = ExprKind::Undefined;
  double number = 0.0;
  bool boolean = false;
  /// `a?.b` / `a?.[k]` / `f?.(x)`: this step short-circuits on a nullish receiver.
  bool optional = false;
  std::string text;                 // identifier / string literal / operator
  std::vector<ExprPtr> children;    // operands, elements, arguments, template parts
  std::vector<PropertyDef> properties;
  FunctionPtr function;             // for Function expressions
  std::shared_ptr<ClassNode> class_node;  // for Class expressions
};

enum class StmtKind {
  Expression,
  Var,
  Block,
  If,
  While,
  DoWhile,
  For,
  ForOf,
  ForIn,
  FunctionDecl,
  ClassDecl,
  Return,
  Break,
  Continue,
  Throw,
  Try,
  With,      // `with (object) body` — sloppy code only
  Labeled,   // `label: body` (the label is in `declaration_kind`)
  Switch,    // `switch (expression) { case … }` — clauses are in `cases`
  Empty,
};

/// One `case test:` / `default:` clause of a `switch` statement.
struct SwitchCase {
  ExprPtr test;                     // null for `default`
  std::vector<StmtPtr> statements;
};

struct Stmt {
  StmtKind kind = StmtKind::Empty;

  std::string declaration_kind;     // "var" / "let" / "const"
  std::vector<std::string> names;   // declared names
  std::vector<ExprPtr> values;      // initialisers, parallel to `names`

  ExprPtr expression;               // expression statement / condition / update
  StmtPtr body;                     // loop or if-then
  StmtPtr alternate;                // else branch
  StmtPtr initializer;              // for(;;) initialiser statement
  StmtPtr update_statement;         // for(;;) update expression

  std::vector<StmtPtr> statements;  // program / block contents
  FunctionPtr function;             // function declaration
  std::shared_ptr<ClassNode> class_node;  // class declaration

  std::vector<SwitchCase> cases;    // switch clauses, in source order

  StmtPtr try_block;
  StmtPtr catch_block;
  std::string catch_name;
  StmtPtr finally_block;

  bool is_for_of = false;           // for-of vs for-in
  bool declares_iteration = false;  // `for (let x of …)`
  bool is_strict = false;           // program node: the source started with "use strict"
};

struct FunctionNode {
  std::string name;
  std::vector<std::string> parameters;
  std::string rest_parameter;  // `function f(a, ...rest)`; empty when absent
  std::vector<StmtPtr> body;
  bool is_arrow = false;
  bool expression_body = false;  // arrow with a single expression instead of a block
  bool is_strict = false;         // body runs in strict mode (directive or inherited)
};

}  // namespace bonk::js::ast
