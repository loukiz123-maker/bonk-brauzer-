#include "bonk/html/html.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "bonk/core/strings.hpp"
#include "bonk/html/tokenizer.hpp"

namespace bonk::html {
namespace {

const char* const kVoid[] = {"area", "base", "br",  "col",   "embed", "hr",   "img",  "input",
                             "link", "meta", "param", "source", "track", "wbr", "frame", "keygen"};

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

// Start tags that leave the "frameset-ok" flag alone. The spec clears the flag for
// characters and for "any other start tag", which is roughly "inline" content: a
// document that only has blocks so far may still be replaced by a <frameset>.
const char* const kFramesetOkStart[] = {"html",  "head",   "body",     "frameset", "frame",  "noframes", "base",
                                        "basefont", "bgsound", "link", "meta",     "title",  "style",    "script",
                                        "template", "li",   "dd",       "dt",       "form",   "pre",      "listing",
                                        "button", "plaintext", "p",     "div",      "span",   "section",  "article",
                                        "aside", "header", "footer",   "nav",      "main",   "h1",       "h2",
                                        "h3",    "h4",     "h5",       "h6",       "ul",     "ol",       "dl",
                                        "table", "caption", "colgroup", "tbody",   "td",     "tfoot",    "th",
                                        "thead", "tr",     "col",      "center",   "dir",    "menu",     "address",
                                        "blockquote", "details", "dialog", "fieldset", "figcaption", "figure",
                                        "hgroup", "summary", "search",  "textarea", "select", "option",   "optgroup"};

// End tags with their own "in body" rule: generate implied end tags, then pop.
// Everything else goes through "any other end tag", which stops at a special
// element instead of climbing out of it.
const char* const kBlockEndTags[] = {"address",  "article", "aside",  "blockquote", "button",   "center",
                                     "details",  "dialog",  "dir",    "div",        "dl",       "fieldset",
                                     "figcaption", "figure", "footer", "header",    "hgroup",   "listing",
                                     "main",     "menu",    "nav",    "ol",         "pre",      "section",
                                     "summary",  "ul"};

// Start tags that may stay inside the table structure instead of being fostered out.
const char* const kTableAllowed[] = {"caption", "colgroup", "col", "tbody", "tfoot",  "thead", "tr",
                                     "td",      "th",       "style", "script", "template", "table", "form"};

// "Formatting" elements: the ones the adoption agency algorithm works on.
const char* const kFormatting[] = {"a",  "b",   "big", "code", "em",    "font", "i",
                                   "nobr", "s", "small", "strike", "strong", "tt", "u"};

// The "special" category, used to find the furthest block in the adoption agency.
const char* const kSpecial[] = {"address", "applet",  "area",   "article", "aside",  "base",     "basefont",
                                "bgsound", "blockquote", "body", "br",    "button", "caption",  "center",
                                "col",     "colgroup", "dd",    "details", "dir",    "div",      "dl",
                                "dt",      "embed",    "fieldset", "figcaption", "figure", "footer", "form",
                                "frame",   "frameset", "h1",    "h2",      "h3",     "h4",       "h5",
                                "h6",      "head",     "header", "hgroup",  "hr",     "html",     "iframe",
                                "img",     "input",    "keygen", "li",     "link",   "listing",  "main",
                                "marquee", "menu",     "meta",   "nav",     "noembed", "noframes", "noscript",
                                "object",  "ol",       "p",      "param",   "plaintext", "pre",   "script",
                                "search",  "section",  "select", "source",  "style",  "summary",  "table",
                                "tbody",   "td",       "template", "textarea", "tfoot", "th",      "thead",
                                "title",   "tr",       "track",  "ul",      "wbr",    "xmp"};

const char* const kHeadElements[] = {"base", "basefont", "bgsound", "link", "meta",     "title",
                                     "style", "script",   "noscript",  "template"};

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

  /// `<template>` keeps its children in a separate DocumentFragment ("template
  /// contents"), so the "appropriate place for inserting a node" is that fragment
  /// rather than the element itself.
  [[nodiscard]] dom::NodeId template_content(dom::NodeId element) const {
    for (const dom::NodeId child : document_.children(element)) {
      if (document_.type(child) == dom::NodeType::Fragment) return child;
    }
    return dom::NodeId{};
  }

  [[nodiscard]] dom::NodeId insertion_parent() const {
    const dom::NodeId top = current();
    if (tag_of(top) == "template") {
      const dom::NodeId content = template_content(top);
      if (content.valid()) return content;
    }
    return top;
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

  /// True when the "in select" insertion mode applies: an open <select> with no
  /// table/td/th/caption/template boundary above it (a simplified select scope).
  [[nodiscard]] bool select_context() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "select") return true;
      if (tag == "table" || tag == "td" || tag == "th" || tag == "caption" || tag == "template") return false;
    }
    return false;
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

  /// "Generate implied end tags, optionally ignoring `except`" (HTML §13.2.6.4.8):
  /// pops the phrasing elements that never need an explicit end tag. `rb`/`rt`/
  /// `rtc`/`rp` are in the list, which is what makes `<ruby>a<rb>b<rb>c` produce
  /// sibling annotations instead of a nesting ladder.
  void generate_implied_end_tags(std::string_view except = {}) {
    while (!stack_.empty()) {
      const std::string_view top = tag_of(stack_.back());
      if (top == except) break;
      if (top != "dd" && top != "dt" && top != "li" && top != "optgroup" && top != "option" && top != "p" &&
          top != "rb" && top != "rp" && top != "rt" && top != "rtc") {
        break;
      }
      stack_.pop_back();
    }
  }

  void implied_end_tags(std::string_view next) {
    // A block start tag closes the nearest open <p>, even when other phrasing
    // elements are open in between (`<p><b>x<div>`), but never across a scope
    // boundary such as <button>.
    if (contains(kCloseParagraph, next) && p_in_scope()) close_open("p");
    if (next == "button" && has_open("button")) close_open("button");

    if (next == "li") {
      // Only inside the same list: `<ul><li>a<ul><li>b` must nest, not flatten.
      if (tag_in_scope("li", true)) close_open("li");
    } else if (next == "dd" || next == "dt") {
      if (tag_in_scope("dd", true)) close_open("dd");
      if (tag_in_scope("dt", true)) close_open("dt");
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
    } else if (next == "rb" || next == "rtc") {
      generate_implied_end_tags();
    } else if (next == "rt" || next == "rp") {
      generate_implied_end_tags("rtc");  // an <rtc> stays open around its <rt>s
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

  [[nodiscard]] bool in_frameset() const { return has_open("frameset"); }

  /// True when the document's content root is a <frameset> instead of a <body>.
  [[nodiscard]] bool frameset_document() const { return body_.valid() && tag_of(body_) == "frameset"; }

  /// Replaces the (still empty) <body> with a <frameset>, per the "in body" rule
  /// for a frameset start tag.
  void enter_frameset(const Token& token) {
    const dom::NodeId frameset = make_element(token);
    if (body_.valid() && body_ != head_) document_.unlink(body_);
    document_.append_child(html_, frameset);
    stack_.clear();
    stack_.push_back(html_);
    stack_.push_back(frameset);
    body_ = frameset;
  }

  /// True when a `<template>` sits above the innermost table in the stack: the
  /// "in template" rules then own the content, so the implicit tbody/tr/colgroup
  /// fix must not run (a `<td>` inside `<template>` belongs to the template).
  [[nodiscard]] bool template_above_table() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "template") return true;
      if (tag == "table") return false;
    }
    return false;
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

  // --- active formatting elements ----------------------------------------
  //
  // Without this list a document like `<p><b>one<p>two` cannot be built the way
  // the spec says: <b> stays "active" after its paragraph closes and is cloned
  // into the next one. The same list is what the adoption agency algorithm
  // rewrites when an end tag arrives for an element that is not the current node.
  [[nodiscard]] static bool is_formatting(std::string_view tag) { return contains(kFormatting, tag); }
  [[nodiscard]] static bool is_special(std::string_view tag) { return contains(kSpecial, tag); }

  [[nodiscard]] int index_in_stack(dom::NodeId node) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (stack_[i - 1] == node) return static_cast<int>(i - 1);
    }
    return -1;
  }

  [[nodiscard]] int index_in_afe(dom::NodeId node) const {
    for (std::size_t i = afe_.size(); i > 0; --i) {
      if (afe_[i - 1] == node) return static_cast<int>(i - 1);
    }
    return -1;
  }

  [[nodiscard]] bool same_attributes(dom::NodeId a, dom::NodeId b) const {
    const auto left = document_.attributes(a);
    const auto right = document_.attributes(b);
    if (left.size() != right.size()) return false;
    for (const dom::Attribute& attribute : left) {
      if (document_.attribute(b, attribute.name) != attribute.value) return false;
    }
    return true;
  }

  void push_formatting(dom::NodeId node) {
    // "Noah's Ark clause": at most three entries with the same name and attributes
    // after the last marker; the earliest of them is dropped.
    std::size_t same = 0;
    int earliest = -1;
    for (std::size_t i = afe_.size(); i > 0; --i) {
      const dom::NodeId entry = afe_[i - 1];
      if (!entry.valid()) break;  // marker
      if (document_.tag_name(entry) != document_.tag_name(node)) continue;
      if (!same_attributes(entry, node)) continue;
      ++same;
      earliest = static_cast<int>(i - 1);
    }
    if (same >= 3 && earliest >= 0) afe_.erase(afe_.begin() + earliest);
    afe_.push_back(node);
  }

  void push_marker() { afe_.push_back(dom::NodeId{}); }

  void clear_to_last_marker() {
    while (!afe_.empty()) {
      if (!afe_.back().valid()) {
        afe_.pop_back();
        return;
      }
      afe_.pop_back();
    }
  }

  /// Clones an element (same name, same attributes) without its children.
  [[nodiscard]] dom::NodeId clone_element(dom::NodeId node) const {
    const dom::NodeId copy = document_.create_element(document_.tag_name(node));
    for (const dom::Attribute& attribute : document_.attributes(node)) {
      document_.set_attribute(copy, attribute.name, attribute.value);
    }
    return copy;
  }

  /// Re-opens the formatting elements that were closed by an implicit end tag.
  void reconstruct_formatting() {
    if (afe_.empty()) return;
    if (!afe_.back().valid()) return;                 // marker on top
    if (index_in_stack(afe_.back()) >= 0) return;      // already open
    std::size_t index = afe_.size();
    while (index > 0) {
      const dom::NodeId entry = afe_[index - 1];
      if (!entry.valid() || index_in_stack(entry) >= 0) break;
      --index;
    }
    for (; index < afe_.size(); ++index) {
      const dom::NodeId clone = clone_element(afe_[index]);
      document_.append_child(insertion_parent(), clone);
      stack_.push_back(clone);
      afe_[index] = clone;
    }
  }

  /// Moves-ready detach: unlink from the parent but keep the node alive.
  void detach(dom::NodeId node) { document_.unlink(node); }

  void remove_from_stack(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < stack_.size()) {
      stack_.erase(stack_.begin() + index);
    }
  }

  void remove_from_afe(dom::NodeId node) {
    const int index = index_in_afe(node);
    if (index >= 0) afe_.erase(afe_.begin() + index);
  }

  /// Node name, or "" when the id is dead (detached nodes linger in the
  /// active-formatting list until the algorithm cleans them up).
  [[nodiscard]] std::string_view tag_of(dom::NodeId node) const {
    return node.valid() && document_.is_alive(node) ? document_.tag_name(node) : std::string_view{};
  }

  /// WHATWG "adoption agency algorithm" (HTML §13.2.6.4.7).
  ///
  /// The whole point of the algorithm: an end tag for a formatting element that
  /// is *not* the current node must not simply be ignored or pop everything above
  /// it. `1<b>2<i>3</b>4</i>` builds `<b>2<i>3</i></b><i>4</i>`-shaped trees by
  /// cloning the still-active formatting elements into a new home. This is the
  /// single most visible tree-construction rule and the reason a naive builder
  /// produces the "<b> swallows the rest of the page" bug.
  void adoption_agency(std::string_view subject) {
    // 1. A stray </b> when <b> is the current node and not active: just pop it.
    if (!stack_.empty() && tag_of(stack_.back()) == subject && index_in_afe(stack_.back()) < 0) {
      stack_.pop_back();
      return;
    }
    for (int outer = 0; outer < 8; ++outer) {
      // 2. The formatting element: the last matching entry after the last marker.
      int formatting_index = -1;
      for (std::size_t i = afe_.size(); i > 0; --i) {
        const dom::NodeId entry = afe_[i - 1];
        if (!entry.valid()) break;  // marker
        if (tag_of(entry) == subject) {
          formatting_index = static_cast<int>(i - 1);
          break;
        }
      }
      if (formatting_index < 0) {  // 3. no such element: "any other end tag"
        generic_end_tag(subject);
        return;
      }
      const dom::NodeId formatting = afe_[static_cast<std::size_t>(formatting_index)];
      const int formatting_stack_index = index_in_stack(formatting);
      if (formatting_stack_index < 0) {  // 4. not open: forget it
        afe_.erase(afe_.begin() + formatting_index);
        return;
      }
      if (!in_scope(formatting)) return;  // 5. not in scope: ignore the tag

      // 6. The furthest block: the lowest "special" element above the formatting
      //    element (the boundary the misnested content has to be moved into).
      int furthest_index = -1;
      for (std::size_t i = static_cast<std::size_t>(formatting_stack_index) + 1; i < stack_.size(); ++i) {
        if (is_special(tag_of(stack_[i]))) {
          furthest_index = static_cast<int>(i);
          break;
        }
      }
      if (furthest_index < 0) {  // 7. no furthest block: just pop through it
        stack_.resize(static_cast<std::size_t>(formatting_stack_index));
        afe_.erase(afe_.begin() + formatting_index);
        return;
      }
      const dom::NodeId furthest_block = stack_[static_cast<std::size_t>(furthest_index)];
      const dom::NodeId common_ancestor =
          formatting_stack_index > 0 ? stack_[static_cast<std::size_t>(formatting_stack_index) - 1] : document_.root();

      // 8.-15. Walk down from the furthest block towards the formatting element,
      //        cloning every active formatting element on the way and re-parenting
      //        the nodes we already handled under those clones.
      int bookmark = formatting_index;
      dom::NodeId node = furthest_block;
      dom::NodeId last_node = furthest_block;
      int node_index = furthest_index;
      int inner = 0;
      for (;;) {
        ++inner;
        // 13.2: after four steps a node that is in the stack is dropped from it.
        if (inner > 3 && node_index >= 0 && static_cast<std::size_t>(node_index) < stack_.size() &&
            stack_[static_cast<std::size_t>(node_index)] == node) {
          stack_.erase(stack_.begin() + node_index);
        }
        const int above = node_index - 1;
        if (above < 0) break;  // nothing above: give up
        node_index = above;
        node = stack_[static_cast<std::size_t>(node_index)];
        if (node == formatting) break;  // 13.4: reached it
        const int node_afe_index = index_in_afe(node);
        if (node_afe_index < 0) {  // 13.5: not active, drop and keep walking up
          stack_.erase(stack_.begin() + node_index);
          continue;
        }
        // 13.6-13.10: clone, then push everything we already collected into the clone.
        const dom::NodeId clone = clone_element(node);
        afe_[static_cast<std::size_t>(node_afe_index)] = clone;
        stack_[static_cast<std::size_t>(node_index)] = clone;
        if (last_node == furthest_block) bookmark = node_afe_index + 1;
        detach(last_node);
        document_.append_child(clone, last_node);
        last_node = clone;
        node = clone;
      }

      // 16. Place the last node under the common ancestor (not under the furthest
      //     block: that is the whole trick that keeps the tree sane).
      detach(last_node);
      document_.append_child(common_ancestor, last_node);

      // 17.-18. A fresh copy of the formatting element adopts the furthest block's
      //         former children and is appended to the furthest block.
      const dom::NodeId replacement = clone_element(formatting);
      for (const dom::NodeId child : document_.children(furthest_block)) {
        document_.unlink(child);
        document_.append_child(replacement, child);
      }
      document_.append_child(furthest_block, replacement);

      // 19. Swap the formatting element for its replacement in the active list.
      afe_.erase(afe_.begin() + formatting_index);
      if (bookmark > formatting_index) --bookmark;
      bookmark = std::clamp(bookmark, 0, static_cast<int>(afe_.size()));
      afe_.insert(afe_.begin() + bookmark, replacement);

      // 20. Same in the stack of open elements, immediately *below* the furthest
      //     block — the stack grows towards the current node, so that is the slot
      //     right after it (html5lib: index(furthestBlock) + 1).
      remove_from_stack(index_in_stack(formatting));
      const int furthest_now = index_in_stack(furthest_block);
      stack_.insert(stack_.begin() + (furthest_now < 0 ? static_cast<int>(stack_.size()) : furthest_now + 1), replacement);
    }
  }

  /// True when `tag` is open above the nearest scope boundary.
  [[nodiscard]] bool tag_in_scope(std::string_view tag, bool list_scope = false) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view top = tag_of(stack_[i - 1]);
      if (top == tag) return true;
      if (top == "applet" || top == "caption" || top == "html" || top == "table" || top == "td" ||
          top == "th" || top == "marquee" || top == "object" || top == "template") {
        return false;
      }
      if (list_scope && (top == "ol" || top == "ul" || top == "dl")) return false;
    }
    return false;
  }

  /// Scope check used by the adoption agency: <html>, <table>, <td>, …
  [[nodiscard]] bool in_scope(dom::NodeId node) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (stack_[i - 1] == node) return true;
      const std::string_view tag = document_.tag_name(stack_[i - 1]);
      if (tag == "applet" || tag == "caption" || tag == "html" || tag == "table" || tag == "td" ||
          tag == "th" || tag == "marquee" || tag == "object" || tag == "template") {
        return false;
      }
    }
    return false;
  }

  /// Block-level end tags: `<div>`, `<ul>`, `<button>`, … The spec pops them after
  /// generating implied end tags, which is how `</div>` closes a dangling `<p>`.
  void block_end_tag(std::string_view name) {
    if (!tag_in_scope(name)) {  // a scope boundary (<template>, <td>, <table>, …) wins
      report("html.ignored_end_tag", "</" + std::string(name) + "> ignored: <" + std::string(name) +
                                         "> is not in scope");
      return;
    }
    const std::string_view top = stack_.empty() ? std::string_view{} : tag_of(stack_.back());
    generate_implied_end_tags(name);
    if (!close_open(name)) {
      report("html.stray_end_tag", "unmatched </" + std::string(name) + ">");
    } else if (top != name) {
      report("html.misnested_end_tag", "</" + std::string(name) + "> closed <" + std::string(top) + ">");
    }
  }

  /// "Any other end tag" (HTML §13.2.6.4.7, "in body"): walk down the stack from
  /// the current node. A matching element is popped together with everything above
  /// it; a "special" element — `<p>`, `<table>`, `<template>`, … — stops the search
  /// and the tag is ignored. Without that rule `</div>` climbs out of a
  /// `<template>` and `</span>` climbs out of an open `<button>`.
  void generic_end_tag(std::string_view name) {
    if (contains(kBlockEndTags, name)) {
      block_end_tag(name);
      return;
    }
    for (int i = static_cast<int>(stack_.size()) - 1; i >= 0; --i) {
      const dom::NodeId node = stack_[static_cast<std::size_t>(i)];
      if (tag_of(node) == name) {
        generate_implied_end_tags(name);
        const int index = index_in_stack(node);
        if (index >= 0) stack_.resize(static_cast<std::size_t>(index));
        return;
      }
      if (is_special(tag_of(node))) {
        report("html.ignored_end_tag",
               "</" + std::string(name) + "> ignored: <" + std::string(tag_of(node)) + "> is a boundary");
        return;
      }
    }
    report("html.stray_end_tag", "unmatched </" + std::string(name) + ">");
  }

  /// Creates an element with its attributes; `<template>` also gets its contents
  /// fragment, which is where its children will actually land.
  [[nodiscard]] dom::NodeId make_element(const Token& token) {
    const dom::NodeId element = document_.create_element(token.name);
    for (const auto& [key, value] : token.attributes) document_.set_attribute(element, key, value);
    if (token.name == "template") document_.append_child(element, document_.create_fragment());
    return element;
  }

  void insert_element(const Token& token, bool push) {
    const dom::NodeId element = make_element(token);
    document_.append_child(insertion_parent(), element);
    if (push) stack_.push_back(element);
  }

  void append_text(std::string_view text) {
    if (text.empty()) return;
    if (table_context() && !trim(text).empty()) {  // text inside a table is moved out
      foster_insert(document_.create_text(text), true);
      return;
    }
    document_.append_text(insertion_parent(), text);
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
    const std::string_view top = tag_of(current());
    if (top == "frameset" || top == "frame") {  // only whitespace survives here
      if (trim(text).empty()) {
        document_.append_text(current(), text);
      } else {
        report("html.ignored_in_frameset", "text ignored inside <frameset>");
      }
      return;
    }
    // A frameset document has no body: anything after </frameset> is dropped, but
    // <noframes> and <template> keep real content.
    if (frameset_document() && !has_open("noframes") && !has_open("template")) return;
    if (!body_created()) {
      const dom::NodeId top_node = current();
      const bool inside_element = html_.valid() && top_node != html_ && top_node != head_;
      if (inside_element) {  // text inside <title>/<style>/<script> in the head
        append_text(text);
        return;
      }
      // Whitespace between tags before <body> is not rendered by anyone.
      if (trim(text).empty() && !options_.keep_whitespace_in_head) return;
      ensure_body();
    }
    if (!trim(text).empty()) frameset_ok_ = false;
    if (!table_context()) reconstruct_formatting();
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

    if (name == "frameset") {
      if (in_frameset()) {  // a nested frameset is just another child
        insert_element(token, true);
        return;
      }
      if (body_created() && !frameset_ok_) {
        report("html.ignored_frameset", "<frameset> ignored: the body already has content");
        return;
      }
      ensure_html();
      if (!head_created()) {
        ensure_head();
        close_head();
      }
      enter_frameset(token);
      return;
    }

    if (name == "frame" && !in_frameset()) {  // <frame> outside a frameset is dropped
      report("html.stray_frame", "<frame> outside <frameset> ignored");
      return;
    }

    if (in_frameset()) {
      if (name == "frame") {
        insert_element(token, false);
        return;
      }
      if (name != "template" && name != "noframes") {
        report("html.ignored_in_frameset", "<" + std::string(name) + "> ignored inside <frameset>");
        return;
      }
    }

    if (!body_created() && contains(kHeadElements, name)) {
      ensure_head();
      insert_element(token, true);
      if (name == "template") push_marker();
      return;
    }

    if (name == "plaintext") {  // swallows the rest of the document
      if (!body_created()) ensure_body();
      implied_end_tags(name);
      insert_element(token, true);
      tokenizer_->enter_plain_text();
      return;
    }

    if (!contains(kFramesetOkStart, name) && name != "frameset") frameset_ok_ = false;

    // Content of an open <template> is parsed as if it were in <body>, but it must
    // stay inside the template instead of creating the document's <body>.
    if (!body_created() && !has_open("template")) ensure_body();

    // --- "in select" insertion mode ---------------------------------------
    // Inside <select> only option/optgroup/script/template survive; the rest is
    // either ignored or (input/keygen/textarea) forcibly closes the select. Real
    // pages rely on it: `<select><b><option>` must not grow a <b> wrapper.
    if (select_context()) {
      if (name == "option" || name == "optgroup") {
        if (current_is("option")) stack_.pop_back();
        if (name == "optgroup" && current_is("optgroup")) stack_.pop_back();
        insert_element(token, true);
        return;
      }
      if (name == "hr") {
        if (current_is("option")) stack_.pop_back();
        if (current_is("optgroup")) stack_.pop_back();
        insert_element(token, false);
        return;
      }
      if (name == "select") {
        report("html.nested_select", "<select> nested in <select> closes the outer one");
        close_open("select");
        return;
      }
      if (name == "input" || name == "keygen" || name == "textarea") {
        report("html.select_closed_by_input", "<" + std::string(name) + "> inside <select> closes it");
        close_open("select");
      } else if (name != "script" && name != "template") {
        report("html.ignored_in_select", "<" + std::string(name) + "> ignored inside <select>");
        return;
      }
    }

    // --- table fixes: implicit <tbody>/<tr>/<colgroup> -------------------
    if ((name == "td" || name == "th" || name == "tr" || name == "col" || name == "colgroup") &&
        nearest_table().valid() && !template_above_table()) {
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

    // Formatting elements: reconstruct what an implicit end tag closed, then track
    // the new element in the active formatting list.
    if (is_formatting(name)) {
      if (name == "nobr" && has_open("nobr")) {
        adoption_agency("nobr");
        reconstruct_formatting();
      }
      if (name == "a") {
        dom::NodeId open_a;  // copy first: the adoption agency rewrites the list
        for (std::size_t i = afe_.size(); i > 0; --i) {
          if (!afe_[i - 1].valid()) break;
          if (document_.tag_name(afe_[i - 1]) == "a") {
            open_a = afe_[i - 1];
            break;
          }
        }
        if (open_a.valid()) {
          adoption_agency("a");
          remove_from_afe(open_a);
        }
      }
      reconstruct_formatting();
      implied_end_tags(name);
      const dom::NodeId element = make_element(token);
      document_.append_child(insertion_parent(), element);
      stack_.push_back(element);
      push_formatting(element);
      return;
    }

    implied_end_tags(name);
    const bool is_void = contains(kVoid, name);
    insert_element(token, !is_void);
    if (name == "td" || name == "th" || name == "caption" || name == "applet" || name == "object" ||
        name == "marquee" || name == "template") {
      push_marker();
    }
  }

  void handle_end_tag(const Token& token) {
    const std::string_view name = token.name;

    if (in_frameset()) {
      if (name == "frameset") {
        close_open("frameset");  // body_ keeps pointing at it: this is a frameset document
        return;
      }
      if (name != "noframes" && name != "template") {
        report("html.ignored_in_frameset", "</" + std::string(name) + "> ignored inside <frameset>");
        return;
      }
    }
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
    if (select_context()) {
      if (name == "option") {
        if (current_is("option")) stack_.pop_back();
        return;
      }
      if (name == "optgroup") {
        if (stack_.size() >= 2 && current_is("option") && tag_of(stack_[stack_.size() - 2]) == "optgroup") {
          stack_.pop_back();
        }
        if (current_is("optgroup")) stack_.pop_back();
        return;
      }
      if (name == "select") {
        close_open("select");
        return;
      }
      if (name != "template") {  // every other end tag is dropped inside <select>
        report("html.ignored_in_select", "</" + std::string(name) + "> ignored inside <select>");
        return;
      }
    }
    if (name == "template") {
      // Pop everything up to and including the template element, then drop the
      // active-formatting entries that were created inside it (back to the marker).
      for (std::size_t i = stack_.size(); i > 0; --i) {
        if (tag_of(stack_[i - 1]) == "template") {
          stack_.resize(i - 1);
          clear_to_last_marker();
          return;
        }
      }
      report("html.stray_end_tag", "</template> without an open <template>");
      return;
    }
    if (is_formatting(name)) {
      adoption_agency(name);
      return;
    }
    generic_end_tag(name);
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
  /// "frameset-ok": true while the body has no content that a <frameset> could not
  /// replace. `<p><frameset>` therefore still yields a frameset, `<b><frameset>`
  /// does not.
  bool frameset_ok_ = true;
  std::vector<dom::NodeId> stack_;
  std::vector<dom::NodeId> afe_;  // active formatting elements; invalid NodeId = marker
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
