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

// Start tags that close an open <p> (WHATWG "in body", the "close a p element"
// clause). Getting this list right is most of what makes real pages lay out the
// way their authors expect.
const char* const kCloseParagraph[] = {"address", "article", "aside",  "blockquote", "center", "details",
                                       "dialog",  "dir",     "div",    "dl",         "fieldset", "figcaption",
                                       "figure",  "footer",  "form",   "h1",         "h2",       "h3",
                                       "h4",      "h5",      "h6",     "header",     "hgroup",   "hr",
                                       "listing", "main",    "menu",   "nav",        "ol",       "p",
                                       "plaintext", "pre",   "section", "summary",   "table",    "ul",
                                       "xmp"};

// Start tags that may stay inside the table structure instead of being fostered out.
const char* const kTableAllowed[] = {"caption", "colgroup", "col", "tbody", "tfoot",  "thead", "tr",
                                     "td",      "th",       "style", "script", "template", "table", "form"};

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

  /// Indices are stack positions; "in scope" means "above the nearest scope
  /// boundary", which is what stops `<button><p><div>` from closing the outer <p>.
  [[nodiscard]] bool p_in_scope() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = document_.tag_name(stack_[i - 1]);
      if (tag == "p") return true;
      if (tag == "button" || tag == "table" || tag == "td" || tag == "th" || tag == "caption" ||
          tag == "marquee" || tag == "object" || tag == "applet" || tag == "template" || tag == "html") {
        return false;
      }
    }
    return false;
  }

  void implied_end_tags(std::string_view next) {
    // A block start tag closes the nearest open <p>, even when other phrasing
    // elements are open in between (`<p><b>x<div>`), but never across a scope
    // boundary such as <button>.
    if (contains(kCloseParagraph, next) && p_in_scope()) close_open("p");
    if (next == "button" && has_open("button")) close_open("button");

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

  [[nodiscard]] dom::NodeId nearest_table() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (document_.tag_name(stack_[i - 1]) == "table") return stack_[i - 1];
    }
    return dom::NodeId{};
  }

  /// True when text or a stray element here must be foster-parented (i.e. moved
  /// before the enclosing <table>), per the "in table" insertion mode.
  [[nodiscard]] bool table_context() const {
    if (stack_.empty()) return false;
    const std::string_view top = document_.tag_name(stack_.back());
    return top == "table" || top == "tbody" || top == "thead" || top == "tfoot" || top == "tr";
  }

  /// Inserts a node just before the innermost open table, merging text with the
  /// node that already sits there.
  void foster_insert(dom::NodeId child, bool text) {
    const dom::NodeId table = nearest_table();
    const dom::NodeId parent = table.valid() ? document_.parent(table) : current();
    if (!parent.valid()) {
      document_.append_child(current(), child);
      return;
    }
    if (text) {
      const dom::NodeId previous = previous_sibling_of(table);
      if (previous.valid() && document_.type(previous) == dom::NodeType::Text) {
        document_.append_text(previous, document_.text_data(child));
        return;
      }
    }
    document_.insert_before(parent, child, table);
  }

  [[nodiscard]] dom::NodeId previous_sibling_of(dom::NodeId node) const {
    if (!node.valid()) return dom::NodeId{};
    const dom::NodeId parent = document_.parent(node);
    if (!parent.valid()) return dom::NodeId{};
    dom::NodeId previous;
    for (const dom::NodeId child : document_.children(parent)) {
      if (child == node) return previous;
      previous = child;
    }
    return dom::NodeId{};
  }

  void insert_element(const Token& token, bool push) {
    const dom::NodeId element = document_.create_element(token.name);
    for (const auto& [name, value] : token.attributes) document_.set_attribute(element, name, value);
    document_.append_child(current(), element);
    if (push) stack_.push_back(element);
  }

  void append_text(std::string_view text) {
    if (text.empty()) return;
    if (table_context() && !trim(text).empty()) {  // text inside a table is moved out
      foster_insert(document_.create_text(text), true);
      return;
    }
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
        // Only the first doctype, before any element, counts; later ones are ignored.
        if (document_element_seen_) return;
        document_.append_child(document_.root(), document_.create_doctype(token.data));
        return;
      case TokenType::Comment: {
        // Before <html> a comment belongs to the document; between <html> and <head>/<body>
        // it is a child of <html>; afterwards it goes to the current node.
        dom::NodeId target = document_.root();
        if (html_.valid()) {
          target = current();
          if (!head_created() && !body_created()) target = html_;
        }
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
    document_element_seen_ = true;

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

    if (name == "plaintext") {  // swallows the rest of the document
      if (!body_created()) ensure_body();
      implied_end_tags(name);
      insert_element(token, true);
      tokenizer_->enter_plain_text();
      return;
    }

    if (!body_created()) ensure_body();

    // --- table fixes: implicit <tbody>/<tr>/<colgroup> -------------------
    if ((name == "td" || name == "th" || name == "tr" || name == "col" || name == "colgroup") &&
        nearest_table().valid()) {
      // "Clear the stack back to a table context": drop fostered elements that sit
      // above the table, but keep the table/tbody/tr chain that is already there.
      while (!stack_.empty()) {
        const std::string_view top = document_.tag_name(stack_.back());
        if (top == "table" || top == "tbody" || top == "thead" || top == "tfoot" || top == "tr") break;
        stack_.pop_back();
      }
      if (name == "col" && current_is("table")) {
        insert_element(Token{TokenType::StartTag, "colgroup", {}, {}, false}, true);
      } else if (name == "td" || name == "th" || name == "tr") {
        if (current_is("table")) insert_element(Token{TokenType::StartTag, "tbody", {}, {}, false}, true);
        if ((name == "td" || name == "th") && !current_is("tr")) {
          insert_element(Token{TokenType::StartTag, "tr", {}, {}, false}, true);
        }
      } else if (name == "colgroup" && current_is("table")) {
        // allowed directly inside <table>
      }
    }

    // Elements that cannot live inside a table are foster-parented before it.
    if (table_context() && !contains(kTableAllowed, name)) {
      const dom::NodeId element = document_.create_element(token.name);
      for (const auto& [key, value] : token.attributes) document_.set_attribute(element, key, value);
      foster_insert(element, false);
      if (!contains(kVoid, name)) stack_.push_back(element);
      return;
    }

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

  bool document_element_seen_ = false;
  std::vector<dom::NodeId> stack_;
  dom::NodeId html_;
  dom::NodeId head_;
  dom::NodeId body_;
};

}  // namespace

Status parse_html(std::string_view bytes, dom::Document& document, Diagnostics* diagnostics, ParseOptions options) {
  // A leading UTF-8 BOM belongs to the encoding layer: drop it here so the
  // tokenizer itself stays byte-exact (which is what the conformance corpus checks).
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
    bytes.remove_prefix(3);
  }
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
