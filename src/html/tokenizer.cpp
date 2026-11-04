#include "bonk/html/tokenizer.hpp"

#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"
#include "bonk/html/entities.hpp"

namespace bonk::html {
namespace {

std::string normalize_newlines(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  std::size_t i = 0;
  if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xEF &&
      static_cast<unsigned char>(input[1]) == 0xBB && static_cast<unsigned char>(input[2]) == 0xBF) {
    i = 3;  // strip UTF-8 BOM
  }
  for (; i < input.size(); ++i) {
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

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\f'; }

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
  text_element_rcdata_ = rcdata;
  text_element_name_ = std::string(tag_name);
  state_ = rcdata ? State::RcData : State::RawText;
}

bool Tokenizer::is_text_end_tag_at(std::size_t position) const {
  if (input_.compare(position, 2, "</") != 0) return false;
  if (!equals_ci_at(input_, position + 2, text_element_name_)) return false;
  const std::size_t after = position + 2 + text_element_name_.size();
  if (after >= input_.size()) return true;
  const char c = input_[after];
  return c == '>' || c == '/' || is_space(c);
}

void Tokenizer::decode_character_reference(std::string& out) {
  // position_ points just past the '&'.
  if (peek() == '#') {
    advance();
    bool hex = false;
    if (peek() == 'x' || peek() == 'X') {
      hex = true;
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
    if (digits == 0) {  // "&#" with no digits: literal text
      out += "&#";
      return;
    }
    if (peek() == ';') advance();
    if (value == 0 || value > kMaxCodePoint || (value >= 0xD800 && value <= 0xDFFF)) {
      value = kReplacementCharacter;
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
  position_ += consumed;
  if (peek() == ';') advance();
  append_utf8(out, entry->code_point);
  if (entry->second != 0) append_utf8(out, entry->second);
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
          decode_character_reference(character_buffer_);
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
          decode_character_reference(character_buffer_);
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
          state_ = State::Data;
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
        token_.name.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        advance();
        continue;
      }

      case State::BeforeAttributeName: {
        if (at_end()) {
          state_ = State::Data;
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
        state_ = State::AttributeName;
        continue;
      }

      case State::AttributeName: {
        if (at_end()) {
          state_ = State::Data;
          finish_attribute();
          return token_;
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
        attribute_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        advance();
        continue;
      }

      case State::AfterAttributeName: {
        if (at_end()) {
          state_ = State::Data;
          finish_attribute();
          return token_;
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
          state_ = State::Data;
          finish_attribute();
          return token_;
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
          state_ = State::Data;
          finish_attribute();
          return token_;
        }
        const char c = peek();
        if (c == quote) {
          advance();
          state_ = State::AfterAttributeValueQuoted;
          continue;
        }
        if (c == '&') {
          advance();
          decode_character_reference(attribute_value_);
          continue;
        }
        attribute_value_.push_back(c);
        advance();
        continue;
      }

      case State::AttributeValueUnquoted: {
        if (at_end()) {
          state_ = State::Data;
          finish_attribute();
          return token_;
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
          decode_character_reference(attribute_value_);
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
          state_ = State::Data;
          finish_attribute();
          return token_;
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
          token_.self_closing = true;
          state_ = State::Data;
          return token_;
        }
        if (at_end()) {
          state_ = State::Data;
          finish_attribute();
          return token_;
        }
        state_ = State::BeforeAttributeName;
        continue;
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
          // In HTML content, CDATA is a bogus comment (foreign content is out of scope).
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

      case State::CommentStart: {
        if (consume_if(">")) {  // "<!-->" is an empty comment
          state_ = State::Data;
          return token_;
        }
        state_ = State::Comment;
        continue;
      }

      case State::Comment: {
        if (at_end()) {
          state_ = State::Data;
          return token_;
        }
        if (input_.compare(position_, 3, "-->") == 0) {
          position_ += 3;
          state_ = State::Data;
          return token_;
        }
        token_.data.push_back(peek());
        advance();
        continue;
      }

      case State::Doctype: {
        if (at_end()) {
          state_ = State::Data;
          token_.data = to_ascii_lower(trim(token_.data));
          return token_;
        }
        if (peek() == '>') {
          advance();
          state_ = State::Data;
          token_.data = to_ascii_lower(trim(token_.data));
          return token_;
        }
        token_.data.push_back(peek());
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
