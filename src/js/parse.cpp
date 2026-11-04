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
    if ((c >= '0' && c <= '9') || (c == '.' && peek(1) >= '0' && peek(1) <= '9')) {
      token.type = Token::Type::Number;
      std::string digits;
      if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();
        advance();
        while (std::isxdigit(static_cast<unsigned char>(peek())) != 0) digits.push_back(advance());
        token.number = static_cast<double>(std::strtoul(digits.c_str(), nullptr, 16));
      } else {
        while (!at_end() && ((peek() >= '0' && peek() <= '9') || peek() == '.' || peek() == 'e' || peek() == 'E' ||
                             ((peek() == '-' || peek() == '+') && !digits.empty() &&
                              (digits.back() == 'e' || digits.back() == 'E')))) {
          digits.push_back(advance());
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

    static const char* kPunctuators[] = {"===", "!==", ">>>", "**=", "=>", "==", "!=", "<=", ">=", "&&", "||", "??",
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
    if (is("switch") || is("class") || is("import") || is("export")) {
      error = "'" + current().text + "' statements are not supported by this engine yet";
      return nullptr;
    }

    auto statement = make_stmt(StmtKind::Expression);
    statement->expression = parse_expression();
    expect_semicolon();
    return statement;
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
    for (;;) {
      if (!is_identifier()) {
        error = "expected a variable name";
        return statement;
      }
      const std::string name = take().text;
      statement->names.push_back(name);
      if (match("=")) {
        statement->values.push_back(parse_assignment());
      } else {
        statement->values.push_back(nullptr);
      }
      if (!match(",")) break;
    }
    expect_semicolon();
    return statement;
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
    while (!is(")") && current().type != Token::Type::EndOfFile) {
      if (is_identifier()) {
        function->parameters.push_back(take().text);
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
      return function;
    }
    StmtPtr body = parse_block();
    function->body = body->statements;
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
      if (!is_identifier()) {
        error = "expected a loop variable";
        return make_stmt(StmtKind::Empty);
      }
      const std::string name = take().text;
      if (is("of") || is("in")) {
        const bool is_of = is("of");
        take();
        auto statement = make_stmt(is_of ? StmtKind::ForOf : StmtKind::ForIn);
        statement->declaration_kind = kind;
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

  ExprPtr parse_assignment() {
    ExprPtr left = parse_binary(0);
    if (is("=") || is("+=") || is("-=") || is("*=") || is("/=") || is("%=")) {
      const std::string op = take().text;
      auto assign = make(ExprKind::Assign);
      assign->text = op;
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
      return update;
    }
    return expression;
  }

  ExprPtr parse_call_member() {
    ExprPtr expression = is("new") ? parse_new() : parse_primary();
    for (;;) {
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
          call->children.push_back(parse_assignment());
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
        expression->children.push_back(parse_assignment());
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
            array->children.push_back(parse_assignment());
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
            if (is_identifier() || current().type == Token::Type::Keyword) {
              property.key = take().text;
            } else if (current().type == Token::Type::String) {
              property.key = take().text;
            } else {
              if (error.empty()) error = "expected a property name";
              break;
            }
            if (match(":")) {
              property.value = parse_assignment();
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
