#include "bonk/html/tokenizer.hpp"

#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"
#include "bonk/html/entities.hpp"

namespace bonk::html {
namespace {

// Only CRLF/CR normalisation happens here; stripping a leading BOM belongs to the
// encoding layer (see html::parse_html), so the tokenizer sees exactly the bytes
// the corpus feeds it.
std::string normalize_newlines(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (c == '\r') {
      if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
      out.push_back('\n');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// WHATWG "numeric character reference end state" table for U+0080..U+009F.
char32_t windows_1252(char32_t value) noexcept {
  switch (value) {
    case 0x80: return 0x20AC;
    case 0x82: return 0x201A;
    case 0x83: return 0x0192;
    case 0x84: return 0x201E;
    case 0x85: return 0x2026;
    case 0x86: return 0x2020;
    case 0x87: return 0x2021;
    case 0x88: return 0x02C6;
    case 0x89: return 0x2030;
    case 0x8A: return 0x0160;
    case 0x8B: return 0x2039;
    case 0x8C: return 0x0152;
    case 0x8E: return 0x017D;
    case 0x91: return 0x2018;
    case 0x92: return 0x2019;
    case 0x93: return 0x201C;
    case 0x94: return 0x201D;
    case 0x95: return 0x2022;
    case 0x96: return 0x2013;
    case 0x97: return 0x2014;
    case 0x98: return 0x02DC;
    case 0x99: return 0x2122;
    case 0x9A: return 0x0161;
    case 0x9B: return 0x203A;
    case 0x9C: return 0x0153;
    case 0x9E: return 0x017E;
    case 0x9F: return 0x0178;
    default: return value;  // 0x81, 0x8D, 0x8F, 0x90, 0x9D stay as they are
  }
}

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\f'; }
char ascii_lower(char c) noexcept {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ci_at(std::string_view input, std::size_t position, std::string_view literal) {
  if (position + literal.size() > input.size()) return false;
  for (std::size_t i = 0; i < literal.size(); ++i) {
    const char a = input[position + i];
    const char b = literal[i];
    const char lower_a = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
    const char lower_b = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
    if (lower_a != lower_b) return false;
  }
  return true;
}

}  // namespace

Tokenizer::Tokenizer(std::string_view input, Diagnostics* diagnostics)
    : input_(normalize_newlines(input)), diagnostics_(diagnostics) {}

void Tokenizer::advance() noexcept {
  if (position_ < input_.size()) ++position_;
}

bool Tokenizer::consume_if(std::string_view literal) noexcept {
  if (input_.compare(position_, literal.size(), literal) != 0) return false;
  position_ += literal.size();
  return true;
}

void Tokenizer::start_new_tag(TokenType type) {
  token_ = Token{};
  token_.type = type;
  token_.name.clear();
  attribute_name_.clear();
  attribute_value_.clear();
}

void Tokenizer::finish_attribute() {
  if (attribute_name_.empty()) return;
  if (token_.type == TokenType::EndTag) {  // attributes on an end tag are dropped
    attribute_name_.clear();
    attribute_value_.clear();
    return;
  }
  for (const auto& [existing, value] : token_.attributes) {
    (void)value;
    if (existing == attribute_name_) {  // duplicate attribute: first one wins
      attribute_name_.clear();
      attribute_value_.clear();
      return;
    }
  }
  token_.attributes.emplace_back(attribute_name_, attribute_value_);
  attribute_name_.clear();
  attribute_value_.clear();
}

void Tokenizer::emit_character(char32_t code_point) { append_utf8(character_buffer_, code_point); }

Token Tokenizer::flush_characters() {
  Token token;
  token.type = TokenType::Characters;
  token.data = std::move(character_buffer_);
  character_buffer_.clear();
  return token;
}

void Tokenizer::enter_text_element(std::string_view tag_name, bool rcdata) {
  text_element_active_ = true;
  script_mode_ = ScriptMode::Data;
  text_element_rcdata_ = rcdata;
  text_element_name_ = std::string(tag_name);
  state_ = rcdata ? State::RcData : State::RawText;
}

void Tokenizer::enter_context_text_state(bool rcdata) {
  text_element_active_ = false;
  text_element_name_.clear();
  text_element_rcdata_ = rcdata;
  state_ = rcdata ? State::RcData : State::RawText;
}

void Tokenizer::enter_plain_text() {
  text_element_active_ = false;
  state_ = State::PlainText;
}

bool Tokenizer::starts_tag_end(std::size_t position) const {
  if (position >= input_.size()) return false;
  const char c = input_[position];
  return c == '>' || c == '/' || is_space(c);
}

bool Tokenizer::is_text_end_tag_at(std::size_t position) const {
  if (input_.compare(position, 2, "</") != 0) return false;
  if (!equals_ci_at(input_, position + 2, text_element_name_)) return false;
  const std::size_t after = position + 2 + text_element_name_.size();
  // EOF right after the name means the tag never opened: "</xmp" stays text.
  if (after >= input_.size()) return false;
  const char c = input_[after];
  return c == '>' || c == '/' || is_space(c);
}

void Tokenizer::decode_character_reference(std::string& out, bool in_attribute) {
  // position_ points just past the '&'.
  if (peek() == '#') {
    advance();
    bool hex = false;
    char hex_marker = 0;
    if (peek() == 'x' || peek() == 'X') {
      hex = true;
      hex_marker = peek();
      advance();
    }
    char32_t value = 0;
    std::size_t digits = 0;
    while (!at_end()) {
      const char c = peek();
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (hex && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (hex && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      }
      if (digit < 0) break;
      if (value <= 0x10FFFF) value = value * (hex ? 16 : 10) + static_cast<char32_t>(digit);
      advance();
      ++digits;
    }
    if (digits == 0) {  // "&#" / "&#x" with no digits: literal text
      out += "&#";
      if (hex_marker != 0) out.push_back(hex_marker);
      return;
    }
    if (peek() == ';') advance();
    if (value == 0 || value > kMaxCodePoint || (value >= 0xD800 && value <= 0xDFFF)) {
      value = kReplacementCharacter;
    } else if (value >= 0x80 && value <= 0x9F) {
      // Numeric references in the C1 range are interpreted as Windows-1252, for
      // compatibility with pages written against old browsers.
      value = windows_1252(value);
    }
    append_utf8(out, value);
    return;
  }

  std::size_t consumed = 0;
  const std::string_view rest = std::string_view(input_).substr(position_);
  const EntityEntry* entry = find_entity(rest, consumed);
  if (entry == nullptr) {
    out += '&';
    return;
  }
  if (in_attribute && !entry->semicolon) {
    // Historical rule: in an attribute value a legacy reference without ';'
    // followed by '=' or an alphanumeric stays literal (`&noti;`, `&lang=`).
    const char next = rest.size() > consumed ? rest[consumed] : '\0';
    const bool alphanumeric = (next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') ||
                              (next >= 'A' && next <= 'Z') || next == '=';
    if (alphanumeric) {
      out += '&';
      return;
    }
  }
  position_ += consumed;
  if (!entry->semicolon && peek() == ';') advance();
  append_utf8(out, entry->code_point);
  if (entry->second != 0) append_utf8(out, entry->second);
}

Token Tokenizer::emit_doctype(bool force_quirks) {
  if (force_quirks) token_.force_quirks = true;
  state_ = at_end() ? State::EndOfFile : State::Data;
  return token_;
}

Token Tokenizer::next() {
  for (;;) {
    switch (state_) {
      case State::Data: {
        if (at_end()) {
          if (!character_buffer_.empty()) {
            state_ = State::EndOfFile;
            return flush_characters();
          }
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (c == '<') {
          advance();  // consume '<'; TagOpen continues from here
          state_ = State::TagOpen;
          if (!character_buffer_.empty()) return flush_characters();
          continue;
        }
        if (c == '&') {
          advance();
          decode_character_reference(character_buffer_, false);
          continue;
        }
        if (c == '\0') {  // data state: NUL is passed through, the tree builder decides
          advance();
          character_buffer_.push_back(c);
          continue;
        }
        character_buffer_.push_back(c);
        advance();
        continue;
      }

      case State::RcData:
      case State::RawText: {
        if (at_end()) {
          if (!character_buffer_.empty()) {
            state_ = State::Data;
            text_element_active_ = false;
            return flush_characters();
          }
          state_ = State::Data;
          text_element_active_ = false;
          return Token{};
        }
        const bool script_text = state_ == State::RawText && text_element_name_ == "script";
        if (script_text && peek() == '<') {
          // Script data escaped / double escaped, in the compact form the corpus
          // needs: the markers themselves are character data.
          if (script_mode_ != ScriptMode::DoubleEscaped && is_text_end_tag_at(position_)) {
            // fall through to the generic end-tag branch below
          } else if (script_mode_ == ScriptMode::Data && input_.compare(position_, 4, "<!--") == 0) {
            character_buffer_ += "<!--";
            position_ += 4;
            script_mode_ = ScriptMode::Escaped;
            continue;
          } else if (script_mode_ == ScriptMode::Escaped && equals_ci_at(input_, position_ + 1, "script") &&
                     starts_tag_end(position_ + 7)) {
            character_buffer_ += input_.substr(position_, 7);
            position_ += 7;
            script_mode_ = ScriptMode::DoubleEscaped;
            continue;
          } else if (script_mode_ == ScriptMode::DoubleEscaped && input_.compare(position_, 2, "</") == 0 &&
                     equals_ci_at(input_, position_ + 2, "script") && starts_tag_end(position_ + 8)) {
            character_buffer_ += input_.substr(position_, 8);
            position_ += 8;
            script_mode_ = ScriptMode::Escaped;
            continue;
          }
        }
        if (script_text && script_mode_ != ScriptMode::Data &&
            (script_mode_ == ScriptMode::Escaped || script_mode_ == ScriptMode::DoubleEscaped) &&
            input_.compare(position_, 3, "-->") == 0) {
          character_buffer_ += "-->";
          position_ += 3;
          script_mode_ = ScriptMode::Data;
          continue;
        }
        if (peek() == '<' && text_element_active_ && is_text_end_tag_at(position_)) {
          if (!character_buffer_.empty()) {
            return flush_characters();
          }
          text_element_active_ = false;
          state_ = State::TagOpen;
          advance();  // '<' consumed, "/name" follows
          continue;
        }
        if (state_ == State::RcData && peek() == '&') {
          advance();
          decode_character_reference(character_buffer_, false);
          continue;
        }
        if (peek() == '\0') {
          advance();
          emit_character(kReplacementCharacter);
          continue;
        }
        character_buffer_.push_back(peek());
        advance();
        continue;
      }

      case State::PlainText: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return character_buffer_.empty() ? Token{} : flush_characters();
        }
        if (peek() == '\0') {
          advance();
          emit_character(kReplacementCharacter);
          continue;
        }
        character_buffer_.push_back(peek());
        advance();
        continue;
      }

      case State::TagOpen: {
        if (at_end()) {
          character_buffer_.push_back('<');
          state_ = State::Data;
          return flush_characters();
        }
        const char c = peek();
        if (c == '!') {
          advance();
          state_ = State::MarkupDeclarationOpen;
          continue;
        }
        if (c == '/') {
          advance();
          state_ = State::EndTagOpen;
          continue;
        }
        if (is_alpha(c)) {
          start_new_tag(TokenType::StartTag);
          state_ = State::TagName;
          continue;
        }
        if (c == '?') {
          state_ = State::BogusComment;
          token_ = Token{};
          token_.type = TokenType::Comment;
          continue;
        }
        character_buffer_.push_back('<');
        state_ = State::Data;
        continue;
      }

      case State::EndTagOpen: {
        if (at_end()) {
          character_buffer_ += "</";
          state_ = State::Data;
          return flush_characters();
        }
        const char c = peek();
        if (is_alpha(c)) {
          start_new_tag(TokenType::EndTag);
          state_ = State::TagName;
          continue;
        }
        if (c == '>') {
          advance();
          state_ = State::Data;
          continue;
        }
        state_ = State::BogusComment;
        token_ = Token{};
        token_.type = TokenType::Comment;
        continue;
      }

      case State::TagName: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          state_ = State::BeforeAttributeName;
          continue;
        }
        if (c == '/') {
          advance();
          state_ = State::SelfClosingStartTag;
          continue;
        }
        if (c == '>') {
          advance();
          state_ = State::Data;
          finish_attribute();
          return token_;
        }
        if (c == '\0') {
          advance();
          append_utf8(token_.name, kReplacementCharacter);
          continue;
        }
        token_.name.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        advance();
        continue;
      }

      case State::BeforeAttributeName: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '/') {
          advance();
          state_ = State::SelfClosingStartTag;
          continue;
        }
        if (c == '>') {
          advance();
          state_ = State::Data;
          finish_attribute();
          return token_;
        }
        attribute_name_.clear();
        attribute_value_.clear();
        if (c == '=') {  // unexpected-equals-sign-before-attribute-name
          attribute_name_ = "=";
          advance();
        }
        state_ = State::AttributeName;
        continue;
      }

      case State::AttributeName: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          state_ = State::AfterAttributeName;
          continue;
        }
        if (c == '/') {
          advance();
          finish_attribute();
          state_ = State::SelfClosingStartTag;
          continue;
        }
        if (c == '=') {
          advance();
          state_ = State::BeforeAttributeValue;
          continue;
        }
        if (c == '>') {
          advance();
          finish_attribute();
          state_ = State::Data;
          return token_;
        }
        if (c == '\0') {
          advance();
          append_utf8(attribute_name_, kReplacementCharacter);
          continue;
        }
        attribute_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        advance();
        continue;
      }

      case State::AfterAttributeName: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '/') {
          advance();
          finish_attribute();
          state_ = State::SelfClosingStartTag;
          continue;
        }
        if (c == '=') {
          advance();
          state_ = State::BeforeAttributeValue;
          continue;
        }
        if (c == '>') {
          advance();
          finish_attribute();
          state_ = State::Data;
          return token_;
        }
        finish_attribute();
        state_ = State::AttributeName;
        continue;
      }

      case State::BeforeAttributeValue: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '"') {
          advance();
          state_ = State::AttributeValueDoubleQuoted;
          continue;
        }
        if (c == '\'') {
          advance();
          state_ = State::AttributeValueSingleQuoted;
          continue;
        }
        if (c == '>') {
          advance();
          finish_attribute();
          state_ = State::Data;
          return token_;
        }
        state_ = State::AttributeValueUnquoted;
        continue;
      }

      case State::AttributeValueDoubleQuoted:
      case State::AttributeValueSingleQuoted: {
        const char quote = state_ == State::AttributeValueDoubleQuoted ? '"' : '\'';
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (c == quote) {
          advance();
          state_ = State::AfterAttributeValueQuoted;
          continue;
        }
        if (c == '&') {
          advance();
          decode_character_reference(attribute_value_, true);
          continue;
        }
        if (c == '\0') {
          advance();
          append_utf8(attribute_value_, kReplacementCharacter);
          continue;
        }
        attribute_value_.push_back(c);
        advance();
        continue;
      }

      case State::AttributeValueUnquoted: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          finish_attribute();
          state_ = State::BeforeAttributeName;
          continue;
        }
        if (c == '&') {
          advance();
          decode_character_reference(attribute_value_, true);
          continue;
        }
        if (c == '\0') {
          advance();
          append_utf8(attribute_value_, kReplacementCharacter);
          continue;
        }
        if (c == '>') {
          advance();
          finish_attribute();
          state_ = State::Data;
          return token_;
        }
        attribute_value_.push_back(c);
        advance();
        continue;
      }

      case State::AfterAttributeValueQuoted: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        const char c = peek();
        if (is_space(c)) {
          advance();
          finish_attribute();
          state_ = State::BeforeAttributeName;
          continue;
        }
        if (c == '/') {
          advance();
          finish_attribute();
          state_ = State::SelfClosingStartTag;
          continue;
        }
        if (c == '>') {
          advance();
          finish_attribute();
          state_ = State::Data;
          return token_;
        }
        finish_attribute();
        state_ = State::BeforeAttributeName;
        continue;
      }

      case State::SelfClosingStartTag: {
        if (!at_end() && peek() == '>') {
          advance();
          finish_attribute();
          token_.self_closing = token_.type != TokenType::EndTag;
          state_ = State::Data;
          return token_;
        }
        if (at_end()) {
          state_ = State::EndOfFile;
          return Token{};
        }
        state_ = State::BeforeAttributeName;
        continue;
      }

      // CDATA section (foreign content only): everything up to `]]>` is text; an
      // unterminated section runs to end of file.
      case State::CdataSection: {
        const std::size_t end = input_.find("]]>", position_);
        if (end == std::string_view::npos) {
          character_buffer_.append(input_.substr(position_));
          position_ = input_.size();
          state_ = State::EndOfFile;
        } else {
          character_buffer_.append(input_.substr(position_, end - position_));
          position_ = end + 3;
          state_ = State::Data;
        }
        if (!character_buffer_.empty()) return flush_characters();
        continue;  // empty section: nothing to emit, the loop happily moves on
      }

      case State::MarkupDeclarationOpen: {
        if (consume_if("--")) {
          state_ = State::CommentStart;
          token_ = Token{};
          token_.type = TokenType::Comment;
          continue;
        }
        if (equals_ci_at(input_, position_, "doctype")) {
          position_ += 7;
          state_ = State::Doctype;
          token_ = Token{};
          token_.type = TokenType::Doctype;
          continue;
        }
        if (consume_if("[CDATA[")) {
          if (foreign_content_) {
            // Inside SVG/MathML a CDATA section is plain character data.
            state_ = State::CdataSection;
            continue;
          }
          // In HTML content it is a bogus comment that swallows the `[CDATA[`.
          state_ = State::BogusComment;
          token_ = Token{};
          token_.type = TokenType::Comment;
          token_.data = "[CDATA[";
          continue;
        }
        state_ = State::BogusComment;
        token_ = Token{};
        token_.type = TokenType::Comment;
        continue;
      }

      // Comment states, per the WHATWG state machine. `data` is the comment text;
      // `-->`, `--!>` and EOF all close it (EOF without `-->` still yields the
      // comment token, which is what the corpus expects).
      case State::CommentStart: {
        if (consume_if(">")) {  // "<!-->" is an empty comment
          state_ = State::Data;
          return token_;
        }
        if (consume_if("-")) {
          state_ = State::CommentStartDash;
          continue;
        }
        state_ = State::Comment;
        continue;
      }

      case State::CommentStartDash: {
        if (consume_if(">")) {  // "<!--->" is an empty comment
          state_ = State::Data;
          return token_;
        }
        if (at_end()) {  // eof-in-comment: emit what we have
          state_ = State::EndOfFile;
          return token_;
        }
        if (consume_if("-")) {
          state_ = State::CommentEnd;
          continue;
        }
        token_.data.push_back('-');
        state_ = State::Comment;
        continue;
      }

      case State::Comment: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return token_;
        }
        const char c = peek();
        if (c == '-') {
          advance();
          state_ = State::CommentEndDash;
          continue;
        }
        if (c == '\0') {
          advance();
          append_utf8(token_.data, kReplacementCharacter);
          continue;
        }
        token_.data.push_back(c);
        advance();
        continue;
      }

      case State::CommentEndDash: {
        if (at_end()) {  // eof-in-comment before the closing dashes
          state_ = State::EndOfFile;
          return token_;
        }
        if (consume_if("-")) {
          state_ = State::CommentEnd;
          continue;
        }
        token_.data.push_back('-');
        state_ = State::Comment;
        continue;
      }

      case State::CommentEnd: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return token_;
        }
        const char c = peek();
        if (c == '>') {
          advance();
          state_ = State::Data;
          return token_;
        }
        if (c == '!') {
          advance();
          state_ = State::CommentEndBang;
          continue;
        }
        if (c == '-') {  // extra dashes belong to the text
          advance();
          token_.data.push_back('-');
          continue;
        }
        token_.data += "--";
        state_ = State::Comment;
        continue;
      }

      case State::CommentEndBang: {
        if (at_end()) {
          state_ = State::EndOfFile;
          return token_;
        }
        const char c = peek();
        if (c == '>') {
          advance();
          state_ = State::Data;
          return token_;
        }
        if (c == '-') {
          advance();
          token_.data += "--!";
          state_ = State::CommentEndDash;
          continue;
        }
        token_.data += "--!";
        state_ = State::Comment;
        continue;
      }

      // --- DOCTYPE ------------------------------------------------------
      // The full state machine, because the identifiers are part of the token:
      // quirks detection needs them, and `<!DOCTYPE html PUBLIC "…" "…">` has to
      // survive into the DOM (the tree corpus prints both of them).
      case State::Doctype: {  // after "<!DOCTYPE": whitespace before the name
        if (at_end()) return emit_doctype(true);
        if (is_space(peek())) {
          advance();
          state_ = State::BeforeDoctypeName;
          continue;
        }
        state_ = State::BeforeDoctypeName;  // reconsume (also for '>')
        continue;
      }

      case State::BeforeDoctypeName: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '>') {  // parse error: missing-doctype-name
          advance();
          return emit_doctype(true);
        }
        if (c == '\0') {
          advance();
          append_utf8(token_.data, kReplacementCharacter);
        } else {
          advance();
          token_.data.push_back(ascii_lower(c));
        }
        state_ = State::DoctypeName;
        continue;
      }

      case State::DoctypeName: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          state_ = State::AfterDoctypeName;
          continue;
        }
        if (c == '>') {
          advance();
          return emit_doctype(false);
        }
        advance();
        if (c == '\0') {
          append_utf8(token_.data, kReplacementCharacter);
        } else {
          token_.data.push_back(ascii_lower(c));
        }
        continue;
      }

      case State::AfterDoctypeName: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '>') {
          advance();
          return emit_doctype(false);
        }
        if (equals_ci_at(input_, position_, "public")) {
          position_ += 6;
          state_ = State::AfterDoctypePublicKeyword;
          continue;
        }
        if (equals_ci_at(input_, position_, "system")) {
          position_ += 6;
          state_ = State::AfterDoctypeSystemKeyword;
          continue;
        }
        // parse error: invalid-character-sequence-after-doctype-name — the rest of
        // the doctype is swallowed by the bogus state, which still ends at '>'.
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::AfterDoctypePublicKeyword: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          state_ = State::BeforeDoctypePublicIdentifier;
          continue;
        }
        if (c == '"' || c == '\'') {
          advance();
          token_.has_public_identifier = true;
          token_.public_identifier.clear();
          state_ = c == '"' ? State::DoctypePublicIdentifierDoubleQuoted
                            : State::DoctypePublicIdentifierSingleQuoted;
          continue;
        }
        if (c == '>') {  // missing public identifier
          advance();
          return emit_doctype(true);
        }
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::BeforeDoctypePublicIdentifier: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '"' || c == '\'') {
          advance();
          token_.has_public_identifier = true;
          token_.public_identifier.clear();
          state_ = c == '"' ? State::DoctypePublicIdentifierDoubleQuoted
                            : State::DoctypePublicIdentifierSingleQuoted;
          continue;
        }
        if (c == '>') {  // missing public identifier
          advance();
          return emit_doctype(true);
        }
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::DoctypePublicIdentifierDoubleQuoted:
      case State::DoctypePublicIdentifierSingleQuoted: {
        const char quote = state_ == State::DoctypePublicIdentifierDoubleQuoted ? '"' : '\'';
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (c == quote) {
          advance();
          state_ = State::AfterDoctypePublicIdentifier;
          continue;
        }
        if (c == '>') {  // parse error: abrupt-doctype-public-identifier
          advance();
          return emit_doctype(true);
        }
        advance();
        if (c == '\0') {
          append_utf8(token_.public_identifier, kReplacementCharacter);
        } else {
          token_.public_identifier.push_back(c);
        }
        continue;
      }

      case State::AfterDoctypePublicIdentifier: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          state_ = State::BetweenDoctypePublicAndSystemIdentifiers;
          continue;
        }
        if (c == '>') {
          advance();
          return emit_doctype(false);
        }
        if (c == '"' || c == '\'') {
          advance();
          token_.has_system_identifier = true;
          token_.system_identifier.clear();
          state_ = c == '"' ? State::DoctypeSystemIdentifierDoubleQuoted
                            : State::DoctypeSystemIdentifierSingleQuoted;
          continue;
        }
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::BetweenDoctypePublicAndSystemIdentifiers: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '>') {
          advance();
          return emit_doctype(false);
        }
        if (c == '"' || c == '\'') {
          advance();
          token_.has_system_identifier = true;
          token_.system_identifier.clear();
          state_ = c == '"' ? State::DoctypeSystemIdentifierDoubleQuoted
                            : State::DoctypeSystemIdentifierSingleQuoted;
          continue;
        }
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::AfterDoctypeSystemKeyword: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          state_ = State::BeforeDoctypeSystemIdentifier;
          continue;
        }
        if (c == '"' || c == '\'') {
          advance();
          token_.has_system_identifier = true;
          token_.system_identifier.clear();
          state_ = c == '"' ? State::DoctypeSystemIdentifierDoubleQuoted
                            : State::DoctypeSystemIdentifierSingleQuoted;
          continue;
        }
        if (c == '>') {  // missing system identifier
          advance();
          return emit_doctype(true);
        }
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::BeforeDoctypeSystemIdentifier: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '"' || c == '\'') {
          advance();
          token_.has_system_identifier = true;
          token_.system_identifier.clear();
          state_ = c == '"' ? State::DoctypeSystemIdentifierDoubleQuoted
                            : State::DoctypeSystemIdentifierSingleQuoted;
          continue;
        }
        if (c == '>') {  // missing system identifier
          advance();
          return emit_doctype(true);
        }
        token_.force_quirks = true;
        state_ = State::BogusDoctype;
        continue;
      }

      case State::DoctypeSystemIdentifierDoubleQuoted:
      case State::DoctypeSystemIdentifierSingleQuoted: {
        const char quote = state_ == State::DoctypeSystemIdentifierDoubleQuoted ? '"' : '\'';
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (c == quote) {
          advance();
          state_ = State::AfterDoctypeSystemIdentifier;
          continue;
        }
        if (c == '>') {  // parse error: abrupt-doctype-system-identifier
          advance();
          return emit_doctype(true);
        }
        advance();
        if (c == '\0') {
          append_utf8(token_.system_identifier, kReplacementCharacter);
        } else {
          token_.system_identifier.push_back(c);
        }
        continue;
      }

      case State::AfterDoctypeSystemIdentifier: {
        if (at_end()) return emit_doctype(true);
        const char c = peek();
        if (is_space(c)) {
          advance();
          continue;
        }
        if (c == '>') {
          advance();
          return emit_doctype(false);
        }
        // parse error: unexpected-character-after-doctype-system-identifier.
        // (No force-quirks here: the doctype is still well formed.)
        state_ = State::BogusDoctype;
        continue;
      }

      case State::BogusDoctype: {
        // EOF here emits the token as it stands: the bogus state never sets the
        // force-quirks flag by itself (unlike the states that expect a quote).
        if (at_end()) {
          state_ = State::EndOfFile;
          return token_;
        }
        if (peek() == '>') {
          advance();
          return emit_doctype(false);
        }
        advance();
        continue;
      }

      case State::BogusComment: {
        if (at_end()) {
          state_ = State::Data;
          return token_;
        }
        if (peek() == '>') {
          advance();
          state_ = State::Data;
          return token_;
        }
        if (peek() == '\0') {
          advance();
          append_utf8(token_.data, kReplacementCharacter);
          continue;
        }
        token_.data.push_back(peek());
        advance();
        continue;
      }

      case State::EndOfFile:
        return Token{};
    }
  }
}

}  // namespace bonk::html
