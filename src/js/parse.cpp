// Lexer + recursive-descent parser for the Bonk Brauzer JS engine.
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "internal.hpp"

namespace bonk::js {
namespace {

using ast::Expr;
using ast::ExprKind;
using ast::ExprPtr;
using ast::FunctionNode;
using ast::FunctionPtr;
using ast::Stmt;
using ast::StmtKind;
using ast::StmtPtr;

bool is_identifier_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}
bool is_identifier_part(char c) { return is_identifier_start(c) || (c >= '0' && c <= '9'); }

struct Token {
  enum class Type { EndOfFile, Number, String, Template, Identifier, Keyword, Punctuator } type = Type::EndOfFile;
  std::string text;
  double number = 0.0;
  std::size_t line = 1;
  std::size_t column = 1;
  bool newline_before = false;
  bool octal_literal = false;
  bool octal_escape = false;
};

const std::unordered_set<std::string>& keywords() {
  static const std::unordered_set<std::string> set = {
      "var",   "let",     "const",  "function", "return", "if",     "else",    "while",  "do",
      "for",   "break",   "continue", "new",    "typeof", "instanceof", "in",  "this",   "true",
      "false", "null",    "undefined", "throw", "try",    "catch",  "finally", "void",   "delete",
      "of",    "switch",  "case",   "default",  "class",  "extends", "super",  "with",   "yield",
      "async", "await",   "static", "import",   "export"};
  return set;
}

struct Lexer {
  std::string source;
  std::size_t position = 0;
  std::size_t line = 1;
  std::size_t column = 1;
  std::string error;
  /// Set when a numeric literal looks like a legacy octal (`010`) or a string
  /// contains an octal escape (`'\1'`): both are SyntaxErrors in strict code.
  bool saw_octal_literal = false;
  bool saw_octal_escape = false;

  [[nodiscard]] bool at_end() const { return position >= source.size(); }
  char peek(std::size_t offset = 0) const {
    return position + offset < source.size() ? source[position + offset] : '\0';
  }
  char advance() {
    const char c = source[position++];
    if (c == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
    return c;
  }

  void skip_whitespace_and_comments(bool& saw_newline) {
    for (;;) {
      if (at_end()) return;
      const char c = peek();
      if (c == '\n' || c == '\r') {
        saw_newline = true;
        advance();
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
        advance();
        continue;
      }
      if (c == '/' && peek(1) == '/') {
        while (!at_end() && peek() != '\n') advance();
        continue;
      }
      if (c == '/' && peek(1) == '*') {
        advance();
        advance();
        bool closed = false;
        while (!at_end()) {
          if (peek() == '*' && peek(1) == '/') {
            advance();
            advance();
            closed = true;
            break;
          }
          if (peek() == '\n') saw_newline = true;
          advance();
        }
        if (!closed) error = "unterminated block comment";
        continue;
      }
      return;
    }
  }

  Token next(bool& saw_newline) {
    Token token;
    saw_newline = false;
    skip_whitespace_and_comments(saw_newline);
    token.newline_before = saw_newline;
    token.line = line;
    token.column = column;
    saw_octal_literal = false;
    saw_octal_escape = false;
    if (at_end()) {
      token.type = Token::Type::EndOfFile;
      return token;
    }

    const char c = peek();
    // `.5` is a number, but the third dot of `...` followed by a digit is not:
    // `[...5]` must lex as `.` `.` `.` `5`.
    const bool leading_dot_number = c == '.' && peek(1) >= '0' && peek(1) <= '9' &&
                                    !(position > 0 && source[position - 1] == '.');
    if ((c >= '0' && c <= '9') || leading_dot_number) {
      token.type = Token::Type::Number;
      std::string digits;
      if (c == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B' || peek(1) == 'o' ||
                       peek(1) == 'O')) {
        const char radix_indicator = peek(1);
        advance();
        advance();
        const int radix = (radix_indicator == 'x' || radix_indicator == 'X')
                              ? 16
                              : ((radix_indicator == 'b' || radix_indicator == 'B') ? 2 : 8);
        for (;;) {
          const char digit = peek();
          if (digit == '_') {  // numeric separator
            advance();
            continue;
          }
          const bool valid = radix == 16   ? std::isxdigit(static_cast<unsigned char>(digit)) != 0
                             : radix == 2  ? (digit == '0' || digit == '1')
                                           : (digit >= '0' && digit <= '7');
          if (!valid) break;
          digits.push_back(advance());
        }
        token.number = static_cast<double>(std::strtoull(digits.c_str(), nullptr, radix));
      } else {
        bool seen_dot = false;
        while (!at_end() && ((peek() >= '0' && peek() <= '9') || peek() == '.' || peek() == 'e' || peek() == 'E' ||
                             peek() == '_' ||
                             ((peek() == '-' || peek() == '+') && !digits.empty() &&
                              (digits.back() == 'e' || digits.back() == 'E')))) {
          // At most one decimal point belongs to the literal: in `1.5.toString()` the
          // second dot starts a member access, not a malformed number. A dot after an
          // exponent must be a member access too (`1e3.toString()` is valid).
          if (peek() == '.') {
            if (seen_dot || digits.find_first_of("eE") != std::string::npos) break;
            seen_dot = true;
          }
          const char digit = advance();
          if (digit == '_') continue;  // `1_000_000` is `1000000`
          digits.push_back(digit);
        }
        token.number = std::strtod(digits.c_str(), nullptr);
        // A numeric literal may not run straight into an identifier: `1.toString` lexes
        // as `1.` followed by `toString`, which the grammar rejects (25.3.1).
        if (!at_end() && !digits.empty() && digits.back() == '.') {
          const char after = peek();
          if (std::isalpha(static_cast<unsigned char>(after)) != 0 || after == '_' || after == '$' ||
              after == '\\') {
            error = "a numeric literal must not be followed by an identifier";
          }
        }
        // `010` is a legacy octal literal; `0.5`, `0e1` and `08` are not.
        if (digits.size() > 1 && digits[0] == '0' && digits[1] >= '0' && digits[1] <= '9' &&
            digits.find_first_of(".eE") == std::string::npos) {
          saw_octal_literal = true;
          // Sloppy code still *reads* such a literal as octal: `010` is 8, not 10.
          // Digits 8 and 9 make it a NonOctalDecimalIntegerLiteral (`08` is 8).
          if (digits.find_first_of("89") == std::string::npos) {
            token.number = static_cast<double>(std::strtoull(digits.c_str(), nullptr, 8));
          }
        }
      }
      token.text = digits;
      token.octal_literal = saw_octal_literal;
      return token;
    }

    if (c == '"' || c == '\'') {
      const char quote = advance();
      token.type = Token::Type::String;
      std::string value;
      while (!at_end() && peek() != quote) {
        char ch = advance();
        if (ch == '\\' && !at_end()) {
          const char escape = advance();
          switch (escape) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'v': value.push_back('\v'); break;
            case '0': value.push_back('\0'); break;
            case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
              // Legacy octal escape: legal in sloppy code, a SyntaxError in strict.
              saw_octal_escape = true;
              value.push_back(escape);
              break;
            case '\n': break;  // line continuation
            case 'u': {
              std::string hex;
              for (int i = 0; i < 4 && !at_end(); ++i) hex.push_back(advance());
              const auto code_point = static_cast<std::uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
              // UTF-8 encode.
              if (code_point < 0x80) {
                value.push_back(static_cast<char>(code_point));
              } else if (code_point < 0x800) {
                value.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
                value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
              } else {
                value.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
                value.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
              }
              break;
            }
            default: value.push_back(escape); break;
          }
          continue;
        }
        if (ch == '\n') {
          error = "unterminated string literal";
          break;
        }
        value.push_back(ch);
      }
      if (at_end()) {
        error = "unterminated string literal";
      } else {
        advance();  // closing quote
      }
      token.text = value;
      token.octal_escape = saw_octal_escape;
      return token;
    }

    if (c == '`') {
      advance();
      token.type = Token::Type::Template;
      std::string value;
      while (!at_end() && peek() != '`') {
        char ch = advance();
        if (ch == '\\' && !at_end()) {
          const char escape = advance();
          value.push_back(escape == 'n' ? '\n' : (escape == 't' ? '\t' : escape));
          continue;
        }
        value.push_back(ch);
      }
      if (at_end()) error = "unterminated template literal";
      else advance();
      token.text = value;
      token.octal_escape = saw_octal_escape;
      return token;
    }

    if (is_identifier_start(c)) {
      std::string name;
      while (!at_end() && is_identifier_part(peek())) name.push_back(advance());
      token.text = name;
      token.type = keywords().count(name) != 0 ? Token::Type::Keyword : Token::Type::Identifier;
      return token;
    }

    static const char* kPunctuators[] = {"===", "!==", ">>>=", ">>>", "**=", "<<=", ">>=", "&&=", "||=", "?" "?=",
                                         "&=", "|=", "^=", "=>",
                                         "==",  "!=",  "<=",   ">=",  "&&",  "||",  "??",
                                         "?.",  "++",  "--",  "+=",  "-=", "*=", "/=", "%=", "<<", ">>", "**",
        "{",   "}",   "(",   ")",   "[",   "]",   ";",   ",",   "<",   ">",   "+",  "-",  "*",  "/",  "%",
        "&",   "|",   "^",   "!",   "~",   "?",   ":",   "=",   "."};
    for (const char* candidate : kPunctuators) {
      const std::string punctuator(candidate);
      if (source.compare(position, punctuator.size(), punctuator) == 0) {
        token.type = Token::Type::Punctuator;
        token.text = punctuator;
        for (std::size_t i = 0; i < punctuator.size(); ++i) advance();
        return token;
      }
    }

    error = std::string("unexpected character '") + c + "'";
    advance();
    token.type = Token::Type::Punctuator;
    token.text = std::string(1, c);
    return token;
  }
};

struct Parser {
  std::vector<Token> tokens;
  std::size_t index = 0;
  std::string error;

  explicit Parser(std::string_view source) {
    Lexer lexer;
    lexer.source = std::string(source);
    for (;;) {
      bool saw_newline = false;
      Token token = lexer.next(saw_newline);
      if (!lexer.error.empty() && error.empty()) error = lexer.error;
      tokens.push_back(std::move(token));
      if (tokens.back().type == Token::Type::EndOfFile) break;
      if (tokens.size() > 200000) {
        error = "script is too large";
        break;
      }
    }
  }

  [[nodiscard]] const Token& current() const { return tokens[index]; }
  [[nodiscard]] const Token& peek(std::size_t offset = 1) const {
    const std::size_t at = index + offset;
    return at < tokens.size() ? tokens[at] : tokens.back();
  }
  bool is(std::string_view text) const {
    return (current().type == Token::Type::Punctuator || current().type == Token::Type::Keyword) &&
           current().text == text;
  }
  bool is_text(std::string_view text) const { return current().text == text; }
  bool is_identifier() const {
    if (current().type == Token::Type::Identifier) return true;
    // `let`, `static` and `yield` are keywords here only because they are reserved in
    // strict mode; sloppy code is free to use them as ordinary names. (`eval` and
    // `arguments` never reach this path: the lexer already reports them as identifiers
    // and the strict checks reject them by name.)
    return !strict && current().type == Token::Type::Keyword && is_strict_reserved(current().text);
  }
  Token take() { return tokens[index < tokens.size() - 1 ? index++ : index]; }
  bool match(std::string_view text) {
    if (is(text)) {
      take();
      return true;
    }
    return false;
  }
  bool expect(std::string_view text) {
    if (match(text)) return true;
    if (error.empty()) {
      error = "expected '" + std::string(text) + "' but found '" + current().text + "'";
    }
    return false;
  }

  /// Semicolon with automatic semicolon insertion (simplified: newline or '}' ends the statement).
  void expect_semicolon() {
    if (match(";")) return;
    if (current().newline_before || is("}") || current().type == Token::Type::EndOfFile) return;
    if (error.empty()) error = "expected ';' but found '" + current().text + "'";
  }

  ExprPtr make(ExprKind kind) {
    auto expression = std::make_shared<Expr>();
    expression->kind = kind;
    return expression;
  }
  StmtPtr make_stmt(StmtKind kind) {
    auto statement = std::make_shared<Stmt>();
    statement->kind = kind;
    return statement;
  }

  StmtPtr parse_program() {
    auto program = make_stmt(StmtKind::Block);
    directive_position = true;
    while (current().type != Token::Type::EndOfFile && error.empty()) {
      StmtPtr statement = parse_statement();
      if (statement == nullptr) break;
      note_directive(statement);
      program->statements.push_back(std::move(statement));
    }
    program->is_strict = strict;
    return program;
  }

  /// A directive prologue is the run of string-literal statements at the top of a
  /// program or function body; `"use strict"` in it switches the whole code to strict.
  void note_directive(const StmtPtr& statement) {
    if (!directive_position) return;
    if (statement == nullptr || statement->kind != StmtKind::Expression || statement->expression == nullptr ||
        statement->expression->kind != ExprKind::String) {
      directive_position = false;
      return;
    }
    if (statement->expression->text == "use strict") strict = true;
  }

  StmtPtr parse_statement() {
    if (directive_position) {
      // Only a string-literal expression statement can be a directive.
      const bool could_be_directive = error.empty() && current().type == Token::Type::String;
      if (!could_be_directive) directive_position = false;
    }
    StmtPtr statement = parse_statement_inner();
    return statement;
  }

  StmtPtr parse_statement_inner() {
    if (error.empty() && is(";")) {
      take();
      return make_stmt(StmtKind::Empty);
    }
    if (is("{")) return parse_block();
    if (is("var") || is("let") || is("const")) return parse_var_statement();
    if (is("function")) return parse_function_declaration();
    if (is("if")) return parse_if();
    if (is("while")) return parse_while();
    if (is("do")) return parse_do_while();
    if (is("for")) return parse_for();
    if (is("return")) {
      take();
      auto statement = make_stmt(StmtKind::Return);
      if (!current().newline_before && !is(";") && !is("}") && current().type != Token::Type::EndOfFile) {
        statement->expression = parse_expression();
      }
      expect_semicolon();
      return statement;
    }
    if (is("break") || is("continue")) {
      const bool is_break = is("break");
      take();
      auto statement = make_stmt(is_break ? StmtKind::Break : StmtKind::Continue);
      if (is_identifier() && !current().newline_before) statement->declaration_kind = take().text;
      expect_semicolon();
      return statement;
    }
    if (is("throw")) {
      take();
      auto statement = make_stmt(StmtKind::Throw);
      statement->expression = parse_expression();
      expect_semicolon();
      return statement;
    }
    if (is("try")) return parse_try();
    if (is("class")) {
      auto statement = make_stmt(StmtKind::ClassDecl);
      statement->class_node = parse_class();
      return statement;
    }
    if (is("with")) {
      if (strict) {
        error = "'with' is not allowed in strict mode";
        return nullptr;
      }
      error = "'with' statements are not supported by this engine yet";
      return nullptr;
    }
    if (is("switch") || is("import") || is("export")) {
      error = "'" + current().text + "' statements are not supported by this engine yet";
      return nullptr;
    }

    auto statement = make_stmt(StmtKind::Expression);
    statement->expression = parse_expression();
    expect_semicolon();
    return statement;
  }

  /// `class [Name] [extends Expr] { constructor() {} m() {} static s() {} get g() {} }`
  std::shared_ptr<ast::ClassNode> parse_class() {
    auto node = std::make_shared<ast::ClassNode>();
    expect("class");
    if (is_identifier()) node->name = take().text;
    if (is("extends")) {
      take();
      node->super_class = parse_call_member();
    }
    if (!expect("{")) return node;
    // The heritage expression is ordinary code, but everything inside the braces is a
    // ClassBody, which the standard makes strict mode code unconditionally — so methods
    // inherit strictness even when the surrounding script is sloppy.
    const bool outer_strict = strict;
    strict = true;
    while (!is("}") && current().type != Token::Type::EndOfFile && error.empty()) {
      ast::ClassMethod method;
      if (is("static") && !(peek().text == "(")) {
        take();
        method.is_static = true;
      }
      if ((is_text("get") || is_text("set")) && peek().text != "(") {
        method.is_get = is_text("get");
        method.is_set = is_text("set");
        take();
      }
      if (is("[")) {
        take();
        method.computed_key = parse_expression();
        expect("]");
      } else if (is_identifier() || current().type == Token::Type::Keyword ||
                 current().type == Token::Type::String || current().type == Token::Type::Number) {
        method.key = take().text;
      } else {
        if (error.empty()) error = "expected a method name";
        break;
      }
      method.function = parse_function(false, false);
      if (method.is_get || method.is_set) require_accessor_arity(method.function, method.is_get);
      method.function->name = method.key;
      node->methods.push_back(std::move(method));
      match(";");
    }
    strict = outer_strict;
    expect("}");
    return node;
  }

  StmtPtr parse_block(bool function_body = false) {
    auto block = make_stmt(StmtKind::Block);
    const bool outer_directive_position = directive_position;
    // Only a function body may carry a "use strict" directive.
    directive_position = function_body;
    expect("{");
    while (!is("}") && current().type != Token::Type::EndOfFile && error.empty()) {
      StmtPtr statement = parse_statement();
      if (statement == nullptr) break;
      if (function_body) note_directive(statement);
      block->statements.push_back(std::move(statement));
    }
    expect("}");
    directive_position = outer_directive_position;
    return block;
  }

  StmtPtr parse_var_statement() {
    const std::string kind = take().text;
    auto statement = make_stmt(StmtKind::Var);
    statement->declaration_kind = kind;
    // `var [a, b] = …` / `var {a: b} = …` are desugared into several declarators of
    // the same statement, so the initialiser is still evaluated exactly once.
    auto make_temp = [&statement](const ExprPtr& value) {
      const std::string name = "__bonk$v" + std::to_string(statement->names.size());
      statement->names.push_back(name);
      statement->values.push_back(value);
      return identifier_of(name);
    };
    auto bind = [&statement, this](const ExprPtr& target, const ExprPtr& value) {
      if (target == nullptr || target->kind != ExprKind::Identifier) {
        if (error.empty()) error = "invalid destructuring target in declaration";
        return;
      }
      statement->names.push_back(target->text);
      statement->values.push_back(value);
    };
    for (;;) {
      if (is_identifier()) {
        const std::string name = take().text;
        require_valid_binding_name(name, "a variable name");
        statement->names.push_back(name);
        if (match("=")) {
          statement->values.push_back(parse_assignment());
        } else {
          statement->values.push_back(nullptr);
        }
      } else if (is("[") || is("{")) {
        const ExprPtr pattern = parse_call_member();
        const ExprPtr value = match("=") ? parse_assignment() : identifier_of("undefined");
        if (pattern == nullptr || (pattern->kind != ExprKind::Array && pattern->kind != ExprKind::ObjectLiteral)) {
          if (error.empty()) error = "expected a destructuring pattern";
          return statement;
        }
        unpack_pattern(pattern, value, make_temp, bind);
      } else {
        error = "expected a variable name";
        return statement;
      }
      if (!match(",")) break;
    }
    expect_semicolon();
    return statement;
  }

  // ---------------------------------------------------------- destructuring --
  //
  // Patterns are parsed as ordinary array/object literals and unpacked here into
  // plain bindings/assignments; every sub-expression lands in a temporary so it is
  // evaluated exactly once (`var [a] = [f()]` calls `f` once, defaults included).

  std::size_t destructuring_temp = 0;

  /// Names that may not be assigned to or bound in strict code (ES2015 12.1.1).
  static bool is_strict_reserved(const std::string& name) {
    static const std::unordered_set<std::string> reserved = {
        "implements", "interface", "let",   "package",  "private",
        "protected",  "public",    "static", "yield",   "eval",  "arguments"};
    return reserved.count(name) != 0;
  }

  /// Early error for an identifier used as a binding or an assignment target.
  void require_valid_binding_name(const std::string& name, const char* what) {
    if (error.empty() && strict && is_strict_reserved(name)) {
      error = std::string("'") + name + "' is not allowed as " + what + " in strict mode";
    }
  }

  // ------------------------------------------------------------- strict mode --
  bool strict = false;             // current code is strict
  bool directive_position = true;  // next statement may be a directive prologue entry

  std::string new_temp_name() { return "__bonk$d" + std::to_string(destructuring_temp++); }

  static ExprPtr identifier_of(const std::string& name) {
    auto identifier = std::make_shared<ast::Expr>();
    identifier->kind = ast::ExprKind::Identifier;
    identifier->text = name;
    return identifier;
  }

  static ExprPtr literal_of(const std::string& text) {
    auto literal = std::make_shared<ast::Expr>();
    literal->kind = ast::ExprKind::String;
    literal->text = text;
    return literal;
  }

  static ExprPtr number_of(double value) {
    auto literal = std::make_shared<ast::Expr>();
    literal->kind = ast::ExprKind::Number;
    literal->number = value;
    return literal;
  }

  static ExprPtr member_of(const ExprPtr& object, const ExprPtr& key) {
    auto member = std::make_shared<ast::Expr>();
    member->kind = ast::ExprKind::Member;
    member->boolean = true;  // computed
    member->children.push_back(object);
    member->children.push_back(key);
    return member;
  }

  static ExprPtr call_of(const std::string& name, const std::vector<ExprPtr>& arguments) {
    auto call = std::make_shared<ast::Expr>();
    call->kind = ast::ExprKind::Call;
    call->children.push_back(identifier_of(name));
    for (const ExprPtr& argument : arguments) call->children.push_back(argument);
    return call;
  }

  static ExprPtr assign_of(const ExprPtr& target, const ExprPtr& value) {
    auto assign = std::make_shared<ast::Expr>();
    assign->kind = ast::ExprKind::Assign;
    assign->text = "=";
    assign->children.push_back(target);
    assign->children.push_back(value);
    return assign;
  }

  /// `value === undefined ? fallback : value`
  static ExprPtr default_of(const ExprPtr& value, const ExprPtr& fallback) {
    auto compare = std::make_shared<ast::Expr>();
    compare->kind = ast::ExprKind::Binary;
    compare->text = "===";
    compare->children.push_back(value);
    compare->children.push_back(identifier_of("undefined"));
    auto conditional = std::make_shared<ast::Expr>();
    conditional->kind = ast::ExprKind::Conditional;
    conditional->children.push_back(compare);
    conditional->children.push_back(fallback);
    conditional->children.push_back(value);
    return conditional;
  }

  using TempFn = std::function<ExprPtr(const ExprPtr&)>;
  using BindFn = std::function<void(const ExprPtr&, const ExprPtr&)>;

  /// Unpacks `pattern` (array/object literal) from `value`, invoking `bind` for leaves.
  void unpack_pattern(const ExprPtr& pattern, const ExprPtr& value_in, const TempFn& make_temp,
                      const BindFn& bind) {
    if (pattern == nullptr) return;
    if (pattern->kind != ExprKind::Array && pattern->kind != ExprKind::ObjectLiteral) {
      bind(pattern, value_in);
      return;
    }
    // Evaluate the source once: `var [a, b] = make()` must call `make` once.
    const ExprPtr value = make_temp(value_in);
    if (pattern->kind == ExprKind::Array) {
      for (std::size_t i = 0; i < pattern->children.size(); ++i) {
        const ExprPtr& element = pattern->children[i];
        if (element == nullptr) continue;  // `[ , b] = …` hole
        if (element->kind == ExprKind::Spread) {
          bind(element->children[0], call_of("__bonk$arrayRest", {value, number_of(static_cast<double>(i))}));
          return;
        }
        unpack_element(element, member_of(value, number_of(static_cast<double>(i))), make_temp, bind);
      }
      return;
    }
    if (pattern->kind == ExprKind::ObjectLiteral) {
      std::vector<ExprPtr> excluded;
      for (const ast::PropertyDef& property : pattern->properties) {
        if (property.kind == ast::PropertyDef::Kind::Spread) {
          auto keys = std::make_shared<ast::Expr>();
          keys->kind = ast::ExprKind::Array;
          keys->children = excluded;
          bind(property.value, call_of("__bonk$objectRest", {value, keys}));
          continue;
        }
        // `{[k]: v}` — a computed key is folded into a computed member access, so the
        // key expression is evaluated when the binding is initialised.
        const ExprPtr key =
            property.computed_key != nullptr ? property.computed_key : literal_of(property.key);
        excluded.push_back(key);
        unpack_element(property.value, member_of(value, key), make_temp, bind);
      }
      return;
    }
  }

  void unpack_element(const ExprPtr& element, const ExprPtr& slot, const TempFn& make_temp,
                      const BindFn& bind) {
    // `[a = 1]`, `{b: c = 2}` — the default applies to `undefined` only.
    if (element->kind == ExprKind::Assign && element->text == "=" && element->children.size() == 2) {
      const ExprPtr temporary = make_temp(slot);
      unpack_pattern(element->children[0], default_of(temporary, element->children[1]), make_temp, bind);
      return;
    }
    unpack_pattern(element, slot, make_temp, bind);
  }

  /// `get a() {...}` takes no parameters and `set a(v) {...}` takes exactly one.
  void require_accessor_arity(const FunctionPtr& function, bool is_getter) {
    if (!error.empty() || function == nullptr) return;
    const std::size_t count = function->parameters.size();
    if (is_getter) {
      if (count != 0) error = "a getter must not have parameters";
    } else if (count != 1 || !function->rest_parameter.empty()) {
      error = "a setter must have exactly one parameter";
    }
  }

  FunctionPtr parse_function(bool is_arrow, bool require_name) {
    auto function = std::make_shared<FunctionNode>();
    function->is_arrow = is_arrow;
    if (!is_arrow) {
      match("function");
      if (is_identifier()) function->name = take().text;
      else if (require_name && error.empty()) error = "expected a function name";
    }
    if (!expect("(")) return function;
    // Parameter patterns and defaults become statements at the top of the body, so
    // the interpreter only ever sees plain parameter names (stage 30).
    const bool outer_strict = strict;
    std::unordered_set<std::string> seen_parameters;
    std::string duplicate_parameter;
    bool simple_parameters = true;
    std::vector<StmtPtr> prologue;
    auto prologue_temp = [&](const ExprPtr& value) {
      auto declaration = make_stmt(StmtKind::Var);
      const std::string name = "__bonk$t" + std::to_string(prologue.size());
      declaration->names.push_back(name);
      declaration->values.push_back(value);
      prologue.push_back(declaration);
      return identifier_of(name);
    };
    auto prologue_bind = [&](const ExprPtr& target, const ExprPtr& value) {
      if (target == nullptr || target->kind != ExprKind::Identifier) {
        if (error.empty()) error = "invalid destructuring target in parameter";
        return;
      }
      // Names bound by a pattern are parameters too: `function f([a], [a])` repeats `a`.
      if (!seen_parameters.insert(target->text).second) duplicate_parameter = target->text;
      require_valid_binding_name(target->text, "a parameter name");
      auto declaration = make_stmt(StmtKind::Var);
      declaration->names.push_back(target->text);
      declaration->values.push_back(value);
      prologue.push_back(declaration);
    };
    while (!is(")") && current().type != Token::Type::EndOfFile) {
      if (is(".") && peek().text == ".") {
        if (!accept_ellipsis()) break;
        if (!is_identifier()) {
          if (error.empty()) error = "expected a rest parameter name";
          break;
        }
        function->rest_parameter = take().text;
        simple_parameters = false;
        if (!seen_parameters.insert(function->rest_parameter).second) {
          duplicate_parameter = function->rest_parameter;
        }
        require_valid_binding_name(function->rest_parameter, "a parameter name");
        break;
      }
      if (is_identifier()) {
        const std::string name = take().text;
        function->parameters.push_back(name);
        if (!seen_parameters.insert(name).second) duplicate_parameter = name;
        require_valid_binding_name(name, "a parameter name");
        if (is("=")) {
          take();
          simple_parameters = false;
          auto statement = make_stmt(StmtKind::Expression);
          statement->expression =
              assign_of(identifier_of(name), default_of(identifier_of(name), parse_assignment()));
          prologue.push_back(statement);
        }
      } else if (is("[") || is("{")) {
        simple_parameters = false;
        const ExprPtr pattern = parse_call_member();
        const std::string name = "__bonk$p" + std::to_string(function->parameters.size());
        function->parameters.push_back(name);
        ExprPtr source = identifier_of(name);
        if (is("=")) {
          take();
          source = default_of(source, parse_assignment());
        }
        unpack_pattern(pattern, source, prologue_temp, prologue_bind);
      } else {
        error = "expected a parameter name";
        break;
      }
      if (!match(",")) break;
    }
    expect(")");
    if (is_arrow) {
      expect("=>");
      if (is("{")) {
        StmtPtr body = parse_block();
        function->body = body->statements;
      } else {
        auto statement = make_stmt(StmtKind::Return);
        statement->expression = parse_assignment();
        function->body.push_back(statement);
        function->expression_body = true;
      }
      function->body.insert(function->body.begin(), prologue.begin(), prologue.end());
      finish_function_strictness(function, duplicate_parameter, simple_parameters, outer_strict);
      return function;
    }
    StmtPtr body = parse_block(true);
    function->body = body->statements;
    function->body.insert(function->body.begin(), prologue.begin(), prologue.end());
    finish_function_strictness(function, duplicate_parameter, simple_parameters, outer_strict);
    return function;
  }

  /// Applies strict-only rules to a parsed signature, then restores the enclosing
  /// code's strictness (a directive inside a body does not leak out).
  void finish_function_strictness(const FunctionPtr& function, const std::string& duplicate_parameter,
                                  bool simple_parameters, bool outer_strict) {
    // A non-simple parameter list may not repeat names, in either mode (ES2015 14.1.2).
    if (error.empty() && !duplicate_parameter.empty() && (strict || !simple_parameters)) {
      error = "'" + duplicate_parameter + "' is already declared as a parameter";
    }
    if (error.empty() && strict) {
      if (!function->name.empty() && (function->name == "eval" || function->name == "arguments")) {
        error = "'" + function->name + "' is not a valid function name in strict mode";
      }
    }
    function->is_strict = strict;
    strict = outer_strict;
  }

  StmtPtr parse_function_declaration() {
    auto statement = make_stmt(StmtKind::FunctionDecl);
    statement->function = parse_function(false, true);
    return statement;
  }

  StmtPtr parse_if() {
    take();
    auto statement = make_stmt(StmtKind::If);
    expect("(");
    statement->expression = parse_expression();
    expect(")");
    statement->body = parse_statement();
    if (match("else")) statement->alternate = parse_statement();
    return statement;
  }

  StmtPtr parse_while() {
    take();
    auto statement = make_stmt(StmtKind::While);
    expect("(");
    statement->expression = parse_expression();
    expect(")");
    statement->body = parse_statement();
    return statement;
  }

  StmtPtr parse_do_while() {
    take();
    auto statement = make_stmt(StmtKind::DoWhile);
    statement->body = parse_statement();
    expect("while");
    expect("(");
    statement->expression = parse_expression();
    expect(")");
    expect_semicolon();
    return statement;
  }

  StmtPtr parse_for() {
    take();
    const bool is_for_of = false;
    (void)is_for_of;
    expect("(");

    // for (let x of …) / for (var k in …) / classic for(;;)
    if (is("var") || is("let") || is("const")) {
      const std::string kind = take().text;
      ExprPtr pattern = nullptr;
      std::string name;
      if (is("[") || is("{")) {
        pattern = parse_call_member();  // `for (var [k, v] of pairs)`
      } else if (is_identifier()) {
        name = take().text;
      } else {
        error = "expected a loop variable";
        return make_stmt(StmtKind::Empty);
      }
      if (is("of") || is("in")) {
        const bool is_of = is("of");
        take();
        auto statement = make_stmt(is_of ? StmtKind::ForOf : StmtKind::ForIn);
        statement->declaration_kind = kind;
        if (pattern != nullptr) {
          // The loop binds a temporary; the pattern is unpacked at the top of the body.
          const std::string loop_name = "__bonk$loop" + std::to_string(destructuring_temp++);
          statement->names.push_back(loop_name);
          statement->declares_iteration = true;
          statement->expression = parse_expression();
          expect(")");
          StmtPtr body = parse_statement();
          auto wrap = make_stmt(StmtKind::Block);
          auto bindings = make_stmt(StmtKind::Var);
          bindings->declaration_kind = kind;
          const ExprPtr source = call_of("__bonk$copy", {identifier_of(loop_name)});
          auto make_temp = [&bindings](const ExprPtr& slot) {
            const std::string temp = "__bonk$l" + std::to_string(bindings->names.size());
            bindings->names.push_back(temp);
            bindings->values.push_back(slot);
            return identifier_of(temp);
          };
          auto bind = [&bindings, this](const ExprPtr& target, const ExprPtr& slot) {
            if (target == nullptr || target->kind != ExprKind::Identifier) {
              if (error.empty()) error = "invalid destructuring target in loop";
              return;
            }
            bindings->names.push_back(target->text);
            bindings->values.push_back(slot);
          };
          unpack_pattern(pattern, source, make_temp, bind);
          wrap->statements.push_back(bindings);
          wrap->statements.push_back(body);
          statement->body = wrap;
          return statement;
        }
        statement->names.push_back(name);
        statement->declares_iteration = true;
        statement->expression = parse_expression();
        expect(")");
        statement->body = parse_statement();
        return statement;
      }
      auto init = make_stmt(StmtKind::Var);
      init->declaration_kind = kind;
      init->names.push_back(name);
      init->values.push_back(match("=") ? parse_assignment() : nullptr);
      while (match(",")) {
        if (!is_identifier()) break;
        init->names.push_back(take().text);
        init->values.push_back(match("=") ? parse_assignment() : nullptr);
      }
      match(";");
      return finish_classic_for(init);
    }

    if (is(";")) {
      take();
      return finish_classic_for(nullptr);
    }

    ExprPtr first = parse_expression();
    if (is("of") || is("in")) {
      const bool is_of = is("of");
      take();
      auto statement = make_stmt(is_of ? StmtKind::ForOf : StmtKind::ForIn);
      statement->expression = parse_expression();
      statement->values.push_back(first);  // the binding target (identifier or member)
      expect(")");
      statement->body = parse_statement();
      return statement;
    }
    auto init_statement = make_stmt(StmtKind::Expression);
    init_statement->expression = first;
    match(";");
    return finish_classic_for(init_statement);
  }

  StmtPtr finish_classic_for(StmtPtr initializer) {
    auto statement = make_stmt(StmtKind::For);
    statement->initializer = std::move(initializer);
    if (!is(";")) statement->expression = parse_expression();
    expect(";");
    if (!is(")")) {
      auto update = make_stmt(StmtKind::Expression);
      update->expression = parse_expression();
      statement->initializer = statement->initializer;  // keep
      statement->update_statement = update;
    }
    expect(")");
    statement->body = parse_statement();
    return statement;
  }

  StmtPtr parse_try() {
    take();
    auto statement = make_stmt(StmtKind::Try);
    statement->try_block = parse_block();
    if (match("catch")) {
      ExprPtr catch_pattern = nullptr;
      if (match("(")) {
        match("var");  // `catch (var e)` is legacy but appears in the wild
        if (is("[") || is("{")) {
          catch_pattern = parse_call_member();  // `catch ({message})`
        } else if (is_identifier()) {
          statement->catch_name = take().text;
          require_valid_binding_name(statement->catch_name, "a catch parameter");
        } else if (error.empty()) {
          error = "expected a catch parameter";
        }
        expect(")");
      }
      StmtPtr body = parse_block();
      if (catch_pattern != nullptr) {
        // The clause binds a temporary and unpacks the pattern at the top of the body.
        const std::string temp = "__bonk$catch" + std::to_string(destructuring_temp++);
        statement->catch_name = temp;
        auto wrap = make_stmt(StmtKind::Block);
        auto bindings = make_stmt(StmtKind::Var);
        bindings->declaration_kind = "var";
        auto make_temp = [&bindings](const ExprPtr& slot) {
          const std::string name = "__bonk$c" + std::to_string(bindings->names.size());
          bindings->names.push_back(name);
          bindings->values.push_back(slot);
          return identifier_of(name);
        };
        auto bind = [&bindings, this](const ExprPtr& target, const ExprPtr& slot) {
          if (target == nullptr || target->kind != ExprKind::Identifier) {
            if (error.empty()) error = "invalid destructuring target in catch clause";
            return;
          }
          bindings->names.push_back(target->text);
          bindings->values.push_back(slot);
        };
        unpack_pattern(catch_pattern, identifier_of(temp), make_temp, bind);
        wrap->statements.push_back(bindings);
        wrap->statements.push_back(body);
        statement->catch_block = wrap;
      } else {
        statement->catch_block = body;
      }
    }
    if (match("finally")) statement->finally_block = parse_block();
    if (error.empty() && statement->catch_block == nullptr && statement->finally_block == nullptr) {
      error = "try without catch or finally";
    }
    return statement;
  }

  ExprPtr parse_expression() {
    ExprPtr expression = parse_assignment();
    if (!is(",")) return expression;
    auto sequence = make(ExprKind::Sequence);
    sequence->children.push_back(expression);
    while (match(",")) sequence->children.push_back(parse_assignment());
    return sequence;
  }

  static int precedence_of(std::string_view op) {
    if (op == "||" || op == "??") return 1;
    if (op == "&&") return 2;
    if (op == "|") return 3;
    if (op == "^") return 4;
    if (op == "&") return 5;
    if (op == "==" || op == "!=" || op == "===" || op == "!==") return 6;
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "instanceof" || op == "in") return 7;
    if (op == "<<" || op == ">>" || op == ">>>") return 8;
    if (op == "+" || op == "-") return 9;
    if (op == "*" || op == "/" || op == "%") return 10;
    if (op == "**") return 11;
    return -1;
  }

  /// `=`, the arithmetic compounds, the bitwise compounds and `**=`.
  bool is_assignment_operator() const {
    static const char* kOperators[] = {"=",    "+=",  "-=",   "*=",  "/=",  "%=",   "<<=",
                                       ">>=", ">>>=", "&=",  "|=",  "^=",  "**=", "&&=",
                                       "||=", "?" "?="};
    for (const char* candidate : kOperators) {
      if (is(candidate)) return true;
    }
    return false;
  }

  /// Early error: only identifiers and member expressions may be assigned to
  /// (ES2015 12.14.1 AssignmentTargetType). `1 = 2`, `f() = 2`, `this = 1` and
  /// `(a, b) = 1` are SyntaxErrors, not runtime errors.
  static bool is_simple_assignment_target(const ExprPtr& expression) {
    if (expression == nullptr) return false;
    if (expression->kind == ExprKind::Identifier) return true;
    // `a?.b = 1` is an early error (ES2020 13.15.1).
    return expression->kind == ExprKind::Member && !expression->optional;
  }

  /// `=` additionally accepts destructuring patterns, which are (syntactically)
  /// array/object literals. The engine does not implement destructuring yet, so
  /// those still fail at runtime — but they are not early errors.
  static bool is_assignment_target(const ExprPtr& expression) {
    if (is_simple_assignment_target(expression)) return true;
    return expression != nullptr &&
           (expression->kind == ExprKind::Array || expression->kind == ExprKind::ObjectLiteral);
  }

  void require_assignment_target(const ExprPtr& expression, const std::string& what) {
    if (error.empty() && !is_simple_assignment_target(expression)) {
      error = "invalid assignment target in " + what;
    }
  }

  ExprPtr parse_assignment() {
    ExprPtr left = parse_binary(0);
    if (is_assignment_operator()) {
      auto assign = make(ExprKind::Assign);
      assign->text = take().text;
      if (assign->text == "=") {
        if (error.empty() && !is_assignment_target(left)) error = "invalid assignment target in assignment";
      } else {
        require_assignment_target(left, "compound assignment");
      }
      if (error.empty() && strict && left != nullptr && left->kind == ExprKind::Identifier) {
        require_valid_binding_name(left->text, "an assignment target");
      }
      assign->children.push_back(left);
      assign->children.push_back(parse_assignment());
      return assign;
    }
    if (is("?")) {
      take();
      auto conditional = make(ExprKind::Conditional);
      conditional->children.push_back(left);
      conditional->children.push_back(parse_assignment());
      expect(":");
      conditional->children.push_back(parse_assignment());
      return conditional;
    }
    return left;
  }

  ExprPtr parse_binary(int minimum_precedence) {
    ExprPtr left = parse_unary();
    for (;;) {
      const std::string op = current().text;
      const int precedence = precedence_of(op);
      if (precedence < 0 || precedence < minimum_precedence) return left;
      if (current().type != Token::Type::Punctuator && current().type != Token::Type::Keyword) return left;
      take();
      ExprPtr right = parse_binary(precedence + 1);
      const bool logical = op == "&&" || op == "||" || op == "??";
      auto binary = make(logical ? ExprKind::Logical : ExprKind::Binary);
      binary->text = op;
      binary->children.push_back(left);
      binary->children.push_back(right);
      left = binary;
    }
  }

  ExprPtr parse_unary() {
    if (is("!") || is("-") || is("+") || is("~") || is("typeof") || is("void") || is("delete")) {
      auto unary = make(ExprKind::Unary);
      unary->text = take().text;
      const bool is_delete = unary->text == "delete";
      unary->children.push_back(parse_unary());
      if (error.empty() && is_delete && strict && unary->children[0] != nullptr &&
          unary->children[0]->kind == ExprKind::Identifier) {
        error = "delete of an unqualified identifier is not allowed in strict mode";
      }
      return unary;
    }
    if (is("++") || is("--")) {
      auto update = make(ExprKind::Update);
      update->text = take().text;
      update->children.push_back(parse_unary());
      require_assignment_target(update->children[0], "update expression");
      if (error.empty() && strict && update->children[0] != nullptr &&
          update->children[0]->kind == ExprKind::Identifier) {
        require_valid_binding_name(update->children[0]->text, "an update target");
      }
      update->boolean = true;  // prefix
      return update;
    }
    // Arrow function: `(a, b) => …` or `x => …`
    if (is_identifier() && peek().text == "=>") {
      const std::string parameter = take().text;
      auto arrow = make(ExprKind::Function);
      auto function = std::make_shared<FunctionNode>();
      function->is_arrow = true;
      function->parameters.push_back(parameter);
      expect("=>");
      if (is("{")) {
        StmtPtr body = parse_block();
        function->body = body->statements;
      } else {
        auto statement = make_stmt(StmtKind::Return);
        statement->expression = parse_assignment();
        function->body.push_back(statement);
        function->expression_body = true;
      }
      arrow->function = function;
      return arrow;
    }
    if (is("(")) {
      // Look ahead for `() =>` or `(a, b) =>`.
      std::size_t scanned = index;
      int depth = 0;
      bool arrow_ahead = false;
      for (; scanned < tokens.size(); ++scanned) {
        if (tokens[scanned].text == "(") ++depth;
        else if (tokens[scanned].text == ")") {
          --depth;
          if (depth == 0) {
            arrow_ahead = scanned + 1 < tokens.size() && tokens[scanned + 1].text == "=>";
            break;
          }
        } else if (tokens[scanned].text == ";" && depth > 0) {
          break;
        }
      }
      if (arrow_ahead) {
        auto arrow = make(ExprKind::Function);
        arrow->function = parse_function(true, false);
        return arrow;
      }
    }

    ExprPtr expression = parse_call_member();
    if (is("++") || is("--")) {
      auto update = make(ExprKind::Update);
      update->text = take().text;
      update->children.push_back(expression);
      update->boolean = false;  // postfix
      require_assignment_target(expression, "update expression");
      if (error.empty() && strict && expression != nullptr && expression->kind == ExprKind::Identifier) {
        require_valid_binding_name(expression->text, "an update target");
      }
      return update;
    }
    return expression;
  }

  /// `...` is lexed as three `.` punctuators, so look ahead before consuming.
  bool accept_ellipsis() {
    if (!is(".") || peek().text != "." || peek(2).text != ".") {
      if (error.empty()) error = "expected '...'";
      return false;
    }
    take();
    take();
    take();
    return true;
  }

  /// One call argument: `expr` or `...expr`.
  ExprPtr parse_argument() {
    if (is(".") && peek().text == "." ) {
      if (!accept_ellipsis()) return nullptr;
      auto spread = make(ExprKind::Spread);
      spread->children.push_back(parse_assignment());
      return spread;
    }
    return parse_assignment();
  }

  ExprPtr parse_call_member() {
    ExprPtr expression = is("new") ? parse_new() : parse_primary();
    for (;;) {
      if (is("?.")) {
        take();
        if (is("(")) {
          auto call = make(ExprKind::Call);
          call->optional = true;
          call->children.push_back(expression);
          take();
          while (!is(")") && current().type != Token::Type::EndOfFile) {
            call->children.push_back(parse_assignment());
            if (!match(",")) break;
          }
          expect(")");
          expression = call;
          continue;
        }
        auto member = make(ExprKind::Member);
        member->optional = true;
        if (is("[")) {
          take();
          member->boolean = true;
          member->children.push_back(expression);
          member->children.push_back(parse_expression());
          expect("]");
        } else if (is_identifier() || current().type == Token::Type::Keyword) {
          member->text = take().text;
          member->children.push_back(expression);
        } else {
          if (error.empty()) error = "expected a property name after '?.'";
          return expression;
        }
        expression = member;
        continue;
      }
      if (is(".")) {
        take();
        if (!is_identifier() && current().type != Token::Type::Keyword) {
          if (error.empty()) error = "expected a property name";
          return expression;
        }
        auto member = make(ExprKind::Member);
        member->text = take().text;
        member->children.push_back(expression);
        expression = member;
        continue;
      }
      if (is("[")) {
        take();
        auto member = make(ExprKind::Member);
        member->boolean = true;  // computed
        member->children.push_back(expression);
        member->children.push_back(parse_expression());
        expect("]");
        expression = member;
        continue;
      }
      if (is("(")) {
        auto call = make(ExprKind::Call);
        call->children.push_back(expression);
        take();
        while (!is(")") && current().type != Token::Type::EndOfFile) {
          call->children.push_back(parse_argument());
          if (!match(",")) break;
        }
        expect(")");
        expression = call;
        continue;
      }
      return expression;
    }
  }

  ExprPtr parse_new() {
    take();  // 'new'
    auto expression = make(ExprKind::New);
    // `new new Foo()` nests: the callee of the outer `new` is itself a `new` expression.
    ExprPtr callee = is("new") ? parse_new() : parse_primary();
    while (is(".") || is("[")) {
      if (match(".")) {
        auto member = make(ExprKind::Member);
        member->text = take().text;
        member->children.push_back(callee);
        callee = member;
      } else {
        take();
        auto member = make(ExprKind::Member);
        member->boolean = true;
        member->children.push_back(callee);
        member->children.push_back(parse_expression());
        expect("]");
        callee = member;
      }
    }
    expression->children.push_back(callee);
    if (match("(")) {
      while (!is(")") && current().type != Token::Type::EndOfFile) {
        expression->children.push_back(parse_argument());
        if (!match(",")) break;
      }
      expect(")");
    }
    return expression;
  }

  ExprPtr parse_primary() {
    const Token& token = current();
    switch (token.type) {
      case Token::Type::Number: {
        if (strict && token.octal_literal && error.empty()) {
          error = "octal literals are not allowed in strict mode";
        }
        auto expression = make(ExprKind::Number);
        expression->number = token.number;
        take();
        return expression;
      }
      case Token::Type::String: {
        if (strict && token.octal_escape && error.empty()) {
          error = "octal escape sequences are not allowed in strict mode";
        }
        auto expression = make(ExprKind::String);
        expression->text = token.text;
        take();
        return expression;
      }
      case Token::Type::Template: {
        auto expression = make(ExprKind::Template);
        take();
        // The lexer hands us the raw inner text; split on ${...} and parse the parts.
        const std::string& raw = token.text;
        std::string literal;
        std::size_t position = 0;
        while (position < raw.size()) {
          if (raw[position] == '$' && position + 1 < raw.size() && raw[position + 1] == '{') {
            auto text_expression = std::make_shared<Expr>();
            text_expression->kind = ExprKind::String;
            text_expression->text = literal;
            expression->children.push_back(text_expression);
            literal.clear();
            std::size_t depth = 1;
            std::size_t start = position + 2;
            std::size_t scan = start;
            while (scan < raw.size() && depth > 0) {
              if (raw[scan] == '{') ++depth;
              else if (raw[scan] == '}') --depth;
              if (depth == 0) break;
              ++scan;
            }
            const std::string_view inner(raw.data() + start, scan - start);
            Parser inner_parser(inner);
            ExprPtr sub = inner_parser.parse_expression();
            if (!inner_parser.error.empty() && error.empty()) error = inner_parser.error;
            expression->children.push_back(sub);
            position = scan + 1;
            continue;
          }
          literal.push_back(raw[position]);
          ++position;
        }
        if (!literal.empty() || expression->children.empty()) {
          auto text_expression = std::make_shared<Expr>();
          text_expression->kind = ExprKind::String;
          text_expression->text = literal;
          expression->children.push_back(text_expression);
        }
        return expression;
      }
      case Token::Type::Identifier: {
        auto expression = make(ExprKind::Identifier);
        expression->text = token.text;
        take();
        return expression;
      }
      case Token::Type::Keyword: {
        if (token.text == "true" || token.text == "false") {
          auto expression = make(ExprKind::Boolean);
          expression->boolean = token.text == "true";
          take();
          return expression;
        }
        if (token.text == "null") {
          take();
          return make(ExprKind::Null);
        }
        if (token.text == "undefined") {
          take();
          return make(ExprKind::Undefined);
        }
        if (token.text == "this") {
          take();
          return make(ExprKind::This);
        }
        if (token.text == "function") {
          auto expression = make(ExprKind::Function);
          expression->function = parse_function(false, false);
          return expression;
        }
        if (token.text == "class") {
          auto expression = make(ExprKind::Class);
          expression->class_node = parse_class();
          return expression;
        }
        if (token.text == "super") {
          // Resolved at runtime from the class body's `super` binding.
          take();
          auto expression = make(ExprKind::Identifier);
          expression->text = "super";
          return expression;
        }
        // Sloppy code may still read `let`, `static` and `yield` as plain names.
        if (!strict && is_strict_reserved(token.text)) {
          auto expression = make(ExprKind::Identifier);
          expression->text = token.text;
          take();
          return expression;
        }
        if (error.empty()) error = "unexpected keyword '" + token.text + "'";
        take();
        return make(ExprKind::Undefined);
      }
      case Token::Type::Punctuator: {
        if (token.text == "(") {
          take();
          ExprPtr inner = parse_expression();
          expect(")");
          return inner;
        }
        if (token.text == "[") {
          take();
          auto array = make(ExprKind::Array);
          while (!is("]") && current().type != Token::Type::EndOfFile) {
            if (is(",")) {  // elision: `[a, , c]` has a hole
              array->children.push_back(nullptr);
              take();
              continue;
            }
            if (is(".")) {
              // `[...iterable]`
              if (!accept_ellipsis()) break;
              auto spread = make(ExprKind::Spread);
              spread->children.push_back(parse_assignment());
              array->children.push_back(spread);
            } else {
              array->children.push_back(parse_assignment());
            }
            if (!match(",")) break;
          }
          expect("]");
          return array;
        }
        if (token.text == "{") {
          take();
          auto object = make(ExprKind::ObjectLiteral);
          while (!is("}") && current().type != Token::Type::EndOfFile) {
            ast::PropertyDef property;
            bool accessor = false;
            if (is(".") && peek().text == ".") {
              // `{...source}` — copy own enumerable properties.
              if (!accept_ellipsis()) break;
              property.kind = ast::PropertyDef::Kind::Spread;
              property.value = parse_assignment();
              object->properties.push_back(std::move(property));
              if (!match(",")) break;
              continue;
            }
            if (is("[")) {
              // Computed key: `{[k]: v}`, `{[k]() {}}`, `{get [k]() {}}`.
              take();
              property.computed_key = parse_expression();
              expect("]");
              if (is("(")) {
                auto expression = make(ExprKind::Function);
                expression->function = parse_function(false, false);
                property.value = expression;
                accessor = true;
              } else if (match(":")) {
                property.value = parse_assignment();
                accessor = true;  // the value is already parsed: skip the shorthand paths
              } else {
                if (error.empty()) error = "expected ':' after a computed property key";
              }
            } else if (is_identifier() || current().type == Token::Type::Keyword) {
              property.key = take().text;
              // `get x() {}` / `set x(v) {}`: a get/set keyword followed by a
              // property name (not `:` for a data property, not `,`/`}` for shorthand).
              if ((property.key == "get" || property.key == "set") && !is(":") && !is(",") && !is("}") &&
                  current().type != Token::Type::EndOfFile) {
                const bool is_getter = property.key == "get";
                if (is("[")) {
                  take();
                  property.computed_key = parse_expression();
                  expect("]");
                  property.key.clear();
                } else if (is_identifier() || current().type == Token::Type::Keyword ||
                           current().type == Token::Type::String || current().type == Token::Type::Number) {
                  property.key = take().text;
                } else {
                  if (error.empty()) error = "expected a property name after '" + property.key + "'";
                  break;
                }
                property.kind = is_getter ? ast::PropertyDef::Kind::Get : ast::PropertyDef::Kind::Set;
                auto expression = make(ExprKind::Function);
                expression->function = parse_function(false, false);
                require_accessor_arity(expression->function, is_getter);
                expression->function->name = property.key;
                property.value = expression;
                accessor = true;
              }
            } else if (current().type == Token::Type::String ||
                       current().type == Token::Type::Number) {
              // `{1: 'a', 2.5: 'b'}`: numeric property names are ordinary keys.
              property.key = take().text;
            } else {
              if (error.empty()) error = "expected a property name";
              break;
            }
            if (!accessor && is("(")) {
              // Method shorthand: `{ m() { return 1; } }` is a data property whose value
              // is a function named after the key (shorthand methods are not
              // constructors, but this engine has no `new`-target semantics to keep).
              auto expression = make(ExprKind::Function);
              expression->function = parse_function(false, false);
              expression->function->name = property.key;
              property.value = expression;
              accessor = true;
            }
            if (accessor) {
              object->properties.push_back(std::move(property));
              if (!match(",")) break;
              continue;
            }
            if (match(":")) {
              property.value = parse_assignment();
            } else if (is("=")) {
              // Shorthand with a default: `{a = 5}` (used by destructuring).
              take();
              property.value = assign_of(identifier_of(property.key), parse_assignment());
            } else {
              auto identifier = make(ExprKind::Identifier);
              identifier->text = property.key;
              property.value = identifier;
            }
            object->properties.push_back(std::move(property));
            if (!match(",")) break;
          }
          expect("}");
          return object;
        }
        if (error.empty()) error = "unexpected token '" + token.text + "'";
        take();
        return make(ExprKind::Undefined);
      }
      case Token::Type::EndOfFile: {
        if (error.empty()) error = "unexpected end of input";
        return make(ExprKind::Undefined);
      }
    }
    return make(ExprKind::Undefined);
  }
};

}  // namespace

Result<ast::StmtPtr> parse_script(std::string_view source) {
  Parser parser(source);
  StmtPtr program = parser.parse_program();
  if (!parser.error.empty()) {
    return Result<StmtPtr>::failure(Error::make("js.syntax", parser.error + " (at line " +
                                                                  std::to_string(parser.current().line) + ")"));
  }
  return Result<StmtPtr>::success(std::move(program));
}

}  // namespace bonk::js
