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
        while (!at_end() && ((peek() >= '0' && peek() <= '9') || peek() == '.' || peek() == 'e' || peek() == 'E' ||
                             peek() == '_' ||
                             ((peek() == '-' || peek() == '+') && !digits.empty() &&
                              (digits.back() == 'e' || digits.back() == 'E')))) {
          const char digit = advance();
          if (digit == '_') continue;  // `1_000_000` is `1000000`
          digits.push_back(digit);
        }
        token.number = std::strtod(digits.c_str(), nullptr);
      }
      token.text = digits;
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
  bool is_identifier() const { return current().type == Token::Type::Identifier; }
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
    while (current().type != Token::Type::EndOfFile && error.empty()) {
      StmtPtr statement = parse_statement();
      if (statement == nullptr) break;
      program->statements.push_back(std::move(statement));
    }
    return program;
  }

  StmtPtr parse_statement() {
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
      if (is_identifier() || current().type == Token::Type::Keyword || current().type == Token::Type::String ||
          current().type == Token::Type::Number) {
        method.key = take().text;
      } else {
        if (error.empty()) error = "expected a method name";
        break;
      }
      method.function = parse_function(false, false);
      method.function->name = method.key;
      node->methods.push_back(std::move(method));
      match(";");
    }
    expect("}");
    return node;
  }

  StmtPtr parse_block() {
    auto block = make_stmt(StmtKind::Block);
    expect("{");
    while (!is("}") && current().type != Token::Type::EndOfFile && error.empty()) {
      StmtPtr statement = parse_statement();
      if (statement == nullptr) break;
      block->statements.push_back(std::move(statement));
    }
    expect("}");
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
        excluded.push_back(literal_of(property.key));
        unpack_element(property.value, member_of(value, literal_of(property.key)), make_temp, bind);
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
        break;
      }
      if (is_identifier()) {
        const std::string name = take().text;
        function->parameters.push_back(name);
        if (is("=")) {
          take();
          auto statement = make_stmt(StmtKind::Expression);
          statement->expression =
              assign_of(identifier_of(name), default_of(identifier_of(name), parse_assignment()));
          prologue.push_back(statement);
        }
      } else if (is("[") || is("{")) {
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
      return function;
    }
    StmtPtr body = parse_block();
    function->body = body->statements;
    function->body.insert(function->body.begin(), prologue.begin(), prologue.end());
    return function;
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
      if (match("(")) {
        if (is_identifier()) statement->catch_name = take().text;
        expect(")");
      }
      statement->catch_block = parse_block();
    }
    if (match("finally")) statement->finally_block = parse_block();
    if (statement->catch_block == nullptr && statement->finally_block == nullptr) {
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
      unary->children.push_back(parse_unary());
      return unary;
    }
    if (is("++") || is("--")) {
      auto update = make(ExprKind::Update);
      update->text = take().text;
      update->children.push_back(parse_unary());
      require_assignment_target(update->children[0], "update expression");
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
    ExprPtr callee = parse_primary();
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
        auto expression = make(ExprKind::Number);
        expression->number = token.number;
        take();
        return expression;
      }
      case Token::Type::String: {
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
            if (is_identifier() || current().type == Token::Type::Keyword) {
              property.key = take().text;
              // `get x() {}` / `set x(v) {}`: a get/set keyword followed by a
              // property name (not `:` for a data property, not `,`/`}` for shorthand).
              if ((property.key == "get" || property.key == "set") && !is(":") && !is(",") && !is("}") &&
                  current().type != Token::Type::EndOfFile) {
                const bool is_getter = property.key == "get";
                if (is_identifier() || current().type == Token::Type::Keyword) {
                  property.key = take().text;
                } else if (current().type == Token::Type::String) {
                  property.key = take().text;
                } else {
                  if (error.empty()) error = "expected a property name after '" + property.key + "'";
                  break;
                }
                property.kind = is_getter ? ast::PropertyDef::Kind::Get : ast::PropertyDef::Kind::Set;
                auto expression = make(ExprKind::Function);
                expression->function = parse_function(false, false);
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
