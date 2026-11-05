// Bonk Brauzer — HTML tokenizer.
//
// States follow the WHATWG state machine (data, tag, attribute, comment,
// doctype, bogus comment, RCDATA/RAWTEXT/script data), with one deliberate
// simplification: input is pre-normalised (CRLF/CR -> LF, BOM stripped) instead
// of normalising on the fly. Entities are decoded in the tokenizer, as the spec
// requires, so the tree builder only ever sees code points.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bonk/core/diag.hpp"

namespace bonk::html {

enum class TokenType { Doctype, StartTag, EndTag, Comment, Characters, EndOfFile };

struct Token {
  TokenType type = TokenType::EndOfFile;
  std::string name;  // tag name, ASCII-lowercased
  std::vector<std::pair<std::string, std::string>> attributes;
  std::string data;  // character run, comment text, doctype name
  bool self_closing = false;

  // DOCTYPE only: the identifiers between quotes after PUBLIC/SYSTEM. The strings
  // stay empty when the keyword never appeared, so each identifier carries its own
  // "was it there at all" flag — `<!DOCTYPE html PUBLIC "">` has an empty public
  // identifier, `<!DOCTYPE html>` has none.
  std::string public_identifier;
  std::string system_identifier;
  bool has_public_identifier = false;
  bool has_system_identifier = false;
  bool force_quirks = false;  // the tokenizer's "force-quirks" flag (EOF, `>` in a keyword, …)

  [[nodiscard]] std::string_view attribute(std::string_view key) const {
    for (const auto& [name_, value] : attributes) {
      if (name_ == key) return value;
    }
    return {};
  }

  [[nodiscard]] bool has_attribute(std::string_view key) const {
    for (const auto& [name_, value] : attributes) {
      if (name_ == key) return true;
    }
    return false;
  }
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string_view input, Diagnostics* diagnostics = nullptr);

  [[nodiscard]] Token next();

  /// Tree builder calls this after emitting a <script>/<style>/<title> start tag:
  /// the following bytes are text up to the matching end tag, not markup.
  void enter_text_element(std::string_view tag_name, bool rcdata);

  /// Fragment parsing: enter the text state the context element demands, but with
  /// no "appropriate end tag" — the context tag is never emitted as a start tag, so
  /// `</title>` inside the fragment stays text (HTML §13.4, step 4).
  void enter_context_text_state(bool rcdata);

  /// <plaintext> swallows the rest of the document as character data.
  void enter_plain_text();

  /// Start in the CDATA section state. The tree builder reaches the same state from
  /// "markup declaration open" inside foreign content; the html5lib corpus can also
  /// start there directly (`initialStates: ["CDATA section state"]`).
  void enter_cdata_section() { state_ = State::CdataSection; }

  /// The tree builder owns the "adjusted current node", so it tells the tokenizer
  /// whether that node sits in foreign content: `<![CDATA[…]]>` is character data
  /// there and a bogus comment in HTML content (HTML §13.2.5.42).
  void set_foreign_content(bool foreign) { foreign_content_ = foreign; }

 private:
  enum class State {
    Data,
    CdataSection,
    RcData,
    RawText,
    PlainText,
    TagOpen,
    EndTagOpen,
    TagName,
    BeforeAttributeName,
    AttributeName,
    AfterAttributeName,
    BeforeAttributeValue,
    AttributeValueDoubleQuoted,
    AttributeValueSingleQuoted,
    AttributeValueUnquoted,
    AfterAttributeValueQuoted,
    SelfClosingStartTag,
    MarkupDeclarationOpen,
    CommentStart,
    CommentStartDash,
    Comment,
    CommentEndDash,
    CommentEnd,
    CommentEndBang,
    Doctype,  // "DOCTYPE state": the whitespace before the DOCTYPE name
    BeforeDoctypeName,
    DoctypeName,
    AfterDoctypeName,
    AfterDoctypePublicKeyword,
    BeforeDoctypePublicIdentifier,
    DoctypePublicIdentifierDoubleQuoted,
    DoctypePublicIdentifierSingleQuoted,
    AfterDoctypePublicIdentifier,
    BetweenDoctypePublicAndSystemIdentifiers,
    AfterDoctypeSystemKeyword,
    BeforeDoctypeSystemIdentifier,
    DoctypeSystemIdentifierDoubleQuoted,
    DoctypeSystemIdentifierSingleQuoted,
    AfterDoctypeSystemIdentifier,
    BogusDoctype,
    BogusComment,
    EndOfFile,
  };

  void advance() noexcept;
  [[nodiscard]] bool at_end() const noexcept { return position_ >= input_.size(); }
  [[nodiscard]] char peek(std::size_t offset = 0) const noexcept {
    return position_ + offset < input_.size() ? input_[position_ + offset] : '\0';
  }
  bool consume_if(std::string_view literal) noexcept;
  void start_new_tag(TokenType type);
  void finish_attribute();
  void emit_character(char32_t code_point);
  void decode_character_reference(std::string& out, bool in_attribute);
  [[nodiscard]] Token flush_characters();
  /// Ends the DOCTYPE token under construction: `force_quirks` mirrors the spec's
  /// "set the force-quirks flag to on", and the state moves on to data/EOF.
  [[nodiscard]] Token emit_doctype(bool force_quirks);
  [[nodiscard]] bool is_text_end_tag_at(std::size_t position) const;
  [[nodiscard]] bool starts_tag_end(std::size_t position) const;

  std::string input_;
  std::size_t position_ = 0;
  State state_ = State::Data;
  Diagnostics* diagnostics_ = nullptr;

  // current token under construction
  Token token_;
  std::string attribute_name_;
  std::string attribute_value_;
  std::string character_buffer_;
  bool foreign_content_ = false;
  bool text_element_active_ = false;
  bool text_element_rcdata_ = false;
  std::string text_element_name_;
  std::size_t text_element_end_ = 0;

  // <script> has three text modes: plain, "escaped" after `<!--` and
  // "double escaped" after a nested <script>; only the first two honour `</script>`.
  enum class ScriptMode { Data, Escaped, DoubleEscaped };
  ScriptMode script_mode_ = ScriptMode::Data;
};

}  // namespace bonk::html
