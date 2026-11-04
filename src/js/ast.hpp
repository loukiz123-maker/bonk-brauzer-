// Internal AST for the Bonk Brauzer JavaScript engine (not part of the public API).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace bonk::js::ast {

struct Expr;
struct Stmt;
struct FunctionNode;

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
};

struct PropertyDef {
  std::string key;
  ExprPtr value;
};

struct Expr {
  ExprKind kind = ExprKind::Undefined;
  double number = 0.0;
  bool boolean = false;
  std::string text;                 // identifier / string literal / operator
  std::vector<ExprPtr> children;    // operands, elements, arguments, template parts
  std::vector<PropertyDef> properties;
  FunctionPtr function;             // for Function expressions
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
  Return,
  Break,
  Continue,
  Throw,
  Try,
  Empty,
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

  StmtPtr try_block;
  StmtPtr catch_block;
  std::string catch_name;
  StmtPtr finally_block;

  bool is_for_of = false;           // for-of vs for-in
  bool declares_iteration = false;  // `for (let x of …)`
};

struct FunctionNode {
  std::string name;
  std::vector<std::string> parameters;
  std::vector<StmtPtr> body;
  bool is_arrow = false;
  bool expression_body = false;  // arrow with a single expression instead of a block
};

}  // namespace bonk::js::ast
