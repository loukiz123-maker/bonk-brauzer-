#include "bonk/html/html.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "bonk/core/strings.hpp"
#include "bonk/html/tokenizer.hpp"

namespace bonk::html {
namespace {

const char* const kVoid[] = {"area", "base", "br",  "col",   "embed", "hr",   "img",  "input",
                             "link", "meta", "param", "source", "track", "wbr"};

const char* const kCloseParagraph[] = {"address", "article", "aside",  "blockquote", "details", "div",
                                       "dl",      "fieldset", "figcaption", "figure",  "footer",  "form",
                                       "h1",      "h2",     "h3",     "h4",         "h5",      "h6",
                                       "header",  "hr",     "main",   "nav",        "ol",      "p",
                                       "pre",     "section", "table", "ul"};

const char* const kHeadElements[] = {"base", "basefont", "bgsound", "link", "meta", "title", "style",
                                     "script", "noscript"};

const char* const kRawText[] = {"script", "style", "xmp", "iframe", "noembed", "noframes"};
const char* const kRcData[] = {"title", "textarea"};

template <std::size_t N>
bool contains(const char* const (&list)[N], std::string_view value) noexcept {
  for (const char* item : list) {
    if (value == item) return true;
  }
  return false;
}

class TreeBuilder {
 public:
  TreeBuilder(dom::Document& document, Diagnostics* diagnostics, ParseOptions options)
      : document_(document), diagnostics_(diagnostics), options_(options) {}

  void run(std::string_view input) {
    tokenizer_ = std::make_unique<Tokenizer>(input, diagnostics_);
    for (;;) {
      Token token = tokenizer_->next();
      if (token.type == TokenType::EndOfFile) break;
      process(token);
      if (token.type == TokenType::StartTag) {
        if (contains(kRawText, token.name)) tokenizer_->enter_text_element(token.name, false);
        if (contains(kRcData, token.name)) tokenizer_->enter_text_element(token.name, true);
      }
    }
    finish();
  }

 private:
  // --- helpers -----------------------------------------------------------
  [[nodiscard]] dom::NodeId current() const {
    return stack_.empty() ? document_.root() : stack_.back();
  }

  [[nodiscard]] bool has_open(std::string_view tag) const {
    for (const dom::NodeId node : stack_) {
      if (document_.tag_name(node) == tag) return true;
    }
    return false;
  }

  [[nodiscard]] bool current_is(std::string_view tag) const {
    return !stack_.empty() && document_.tag_name(stack_.back()) == tag;
  }

  [[nodiscard]] bool body_created() const { return body_.valid(); }
  [[nodiscard]] bool head_created() const { return head_.valid(); }

  void ensure_html() {
    if (html_.valid()) return;
    html_ = document_.create_element("html");
    document_.append_child(document_.root(), html_);
    stack_.push_back(html_);
  }

  void ensure_head() {
    ensure_html();
    if (head_.valid()) return;
    head_ = document_.create_element("head");
    document_.append_child(html_, head_);
    stack_.push_back(head_);
  }

  void ensure_body() {
    ensure_html();
    if (body_.valid()) return;
    ensure_head();  // every document has a <head>, even when the author omitted it
    close_head();
    body_ = document_.create_element("body");
    document_.append_child(html_, body_);
    stack_.clear();
    stack_.push_back(html_);
    stack_.push_back(body_);
  }

  void close_head() {
    while (!stack_.empty() && stack_.back() != html_) {
      if (stack_.back() == head_) {
        stack_.pop_back();
        return;
      }
      stack_.pop_back();
    }
  }

  /// Pops the stack down to (and including) the nearest `tag`; no-op when absent.
  bool close_open(std::string_view tag) {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (document_.tag_name(stack_[i - 1]) == tag) {
        stack_.resize(i - 1);
        return true;
      }
    }
    return false;
  }

  void implied_end_tags(std::string_view next) {
    if (contains(kCloseParagraph, next) && current_is("p")) close_open("p");

    if (next == "li") {
      close_open("li");
    } else if (next == "dd" || next == "dt") {
      close_open("dd");
      close_open("dt");
    } else if (next == "option") {
      close_open("option");
    } else if (next == "optgroup") {
      close_open("option");
      close_open("optgroup");
    } else if (next == "tr") {
      close_open("td");
      close_open("th");
      close_open("tr");
    } else if (next == "td" || next == "th") {
      close_open("td");
      close_open("th");
    } else if (next.size() == 2 && next[0] == 'h' && next[1] >= '1' && next[1] <= '6') {
      for (const char* heading : {"h1", "h2", "h3", "h4", "h5", "h6"}) close_open(heading);
    }
  }

  void insert_element(const Token& token, bool push) {
    const dom::NodeId element = document_.create_element(token.name);
    for (const auto& [name, value] : token.attributes) document_.set_attribute(element, name, value);
    document_.append_child(current(), element);
    if (push) stack_.push_back(element);
  }

  void append_text(std::string_view text) {
    if (text.empty()) return;
    document_.append_text(current(), text);
  }

  void report(std::string_view code, std::string_view message) {
    if (diagnostics_ == nullptr) return;
    diagnostics_->add(Severity::Warning, SourcePos{}, std::string(code) + ": " + std::string(message));
  }

  // --- token handling ----------------------------------------------------
  void process(const Token& token) {
    switch (token.type) {
      case TokenType::Doctype:
        document_.append_child(document_.root(), document_.create_doctype(token.data));
        return;
      case TokenType::Comment: {
        // Comments before <html> belong to the document, later ones to the current node.
        dom::NodeId target = document_.root();
        if (html_.valid() && (head_created() || body_created())) target = current();
        document_.append_child(target, document_.create_comment(token.data));
        return;
      }
      case TokenType::Characters:
        handle_characters(token.data);
        return;
      case TokenType::StartTag:
        handle_start_tag(token);
        return;
      case TokenType::EndTag:
        handle_end_tag(token);
        return;
      case TokenType::EndOfFile:
        return;
    }
  }

  void handle_characters(std::string_view text) {
    if (!body_created()) {
      const dom::NodeId top = current();
      const bool inside_element = html_.valid() && top != html_ && top != head_;
      if (inside_element) {  // text inside <title>/<style>/<script> in the head
        append_text(text);
        return;
      }
      // Whitespace between tags before <body> is not rendered by anyone.
      if (trim(text).empty() && !options_.keep_whitespace_in_head) return;
      ensure_body();
    }
    append_text(text);
  }

  void handle_start_tag(const Token& token) {
    const std::string_view name = token.name;

    if (name == "html") {
      if (!html_.valid()) {
        ensure_html();
      }
      for (const auto& [key, value] : token.attributes) {
        if (document_.attribute(html_, key).empty()) document_.set_attribute(html_, key, value);
      }
      return;
    }

    if (name == "head") {
      if (head_.valid()) return;
      ensure_head();
      for (const auto& [key, value] : token.attributes) document_.set_attribute(head_, key, value);
      return;
    }

    if (name == "body") {
      if (body_.valid()) {
        for (const auto& [key, value] : token.attributes) {
          if (document_.attribute(body_, key).empty()) document_.set_attribute(body_, key, value);
        }
        return;
      }
      ensure_body();
      for (const auto& [key, value] : token.attributes) document_.set_attribute(body_, key, value);
      return;
    }

    if (!body_created() && contains(kHeadElements, name)) {
      ensure_head();
      insert_element(token, true);
      return;
    }

    if (!body_created()) ensure_body();

    implied_end_tags(name);
    const bool is_void = contains(kVoid, name);
    insert_element(token, !is_void);
  }

  void handle_end_tag(const Token& token) {
    const std::string_view name = token.name;

    if (name == "html" || name == "body") return;  // ignore: the tree is closed at EOF
    if (name == "head") {
      close_head();
      return;
    }
    if (name == "br") {
      insert_element(Token{TokenType::StartTag, "br", {}, {}, true}, false);
      return;
    }
    if (name == "p") {
      if (!close_open("p")) {
        Token synthetic{TokenType::StartTag, "p", {}, {}, false};
        insert_element(synthetic, false);
        report("html.stray_end_tag", "</p> without an open <p>");
      }
      return;
    }
    const std::string_view top = stack_.empty() ? std::string_view{} : document_.tag_name(stack_.back());
    if (!close_open(name)) {
      report("html.stray_end_tag", "unmatched </" + std::string(name) + ">");
    } else if (top != name) {
      report("html.misnested_end_tag", "</" + std::string(name) + "> closed <" + std::string(top) + ">");
    }
  }

  void finish() {
    if (!body_created()) ensure_body();
    if (!head_created()) ensure_head();
    stack_.clear();
  }

  dom::Document& document_;
  Diagnostics* diagnostics_ = nullptr;
  ParseOptions options_;
  std::unique_ptr<Tokenizer> tokenizer_;

  std::vector<dom::NodeId> stack_;
  dom::NodeId html_;
  dom::NodeId head_;
  dom::NodeId body_;
};

}  // namespace

Status parse_html(std::string_view bytes, dom::Document& document, Diagnostics* diagnostics, ParseOptions options) {
  TreeBuilder builder(document, diagnostics, options);
  builder.run(bytes);
  return Status::success();
}

Result<dom::Document> parse_document(std::string_view bytes, std::string_view source_url,
                                    Diagnostics* diagnostics) {
  dom::Document document{std::string(source_url)};
  const Status status = parse_html(bytes, document, diagnostics);
  if (status.is_error()) return Result<dom::Document>::failure(status.error());
  return Result<dom::Document>::success(std::move(document));
}

}  // namespace bonk::html
