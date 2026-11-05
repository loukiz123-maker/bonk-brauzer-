#include "test_framework.hpp"

#include "bonk/dom/dom.hpp"
#include "bonk/dom/selector.hpp"

using namespace bonk;
using namespace bonk::dom;

namespace {

struct Fixture {
  Document document;
  NodeId html;
  NodeId body;
  NodeId div;
  NodeId paragraph;

  Fixture() {
    html = document.create_element("html");
    body = document.create_element("body");
    div = document.create_element("div");
    paragraph = document.create_element("p");
    document.append_child(document.root(), html);
    document.append_child(html, body);
    document.append_child(body, div);
    document.append_child(div, paragraph);
    document.set_attribute(div, "class", "card wide");
    document.set_attribute(div, "id", "main");
    document.set_attribute(paragraph, "class", "note");
    document.append_text(paragraph, "hello ");
    document.append_text(paragraph, "world");
  }
};

}  // namespace

BONK_TEST(dom, tree_construction_and_navigation) {
  Fixture f;
  CHECK_EQ(f.document.children(f.document.root()).size(), std::size_t{1});
  CHECK_EQ(f.document.parent(f.div), f.body);
  CHECK_EQ(f.document.tag_name(f.div), std::string_view("div"));
  CHECK_EQ(f.document.first_child(f.div), f.paragraph);
  CHECK_EQ(f.document.descendants(f.document.root()).size(), std::size_t{5});  // html, body, div, p, text
  CHECK_EQ(f.document.document_element(), f.html);
  CHECK_EQ(f.document.body(), f.body);
}

BONK_TEST(dom, text_nodes_merge) {
  Fixture f;
  const std::vector<NodeId> kids = f.document.children(f.paragraph);
  REQUIRE(kids.size() == 1);
  CHECK_EQ(f.document.text_content(f.paragraph), std::string("hello world"));
}

BONK_TEST(dom, attributes_and_classes) {
  Fixture f;
  CHECK_EQ(f.document.attribute(f.div, "id"), std::string_view("main"));
  CHECK_EQ(f.document.attribute(f.div, "missing"), std::string_view(""));
  CHECK(f.document.has_class(f.div, "card"));
  CHECK(f.document.has_class(f.div, "wide"));
  CHECK(!f.document.has_class(f.div, "narrow"));
  CHECK(!f.document.has_class(f.paragraph, "card"));
  CHECK_EQ(f.document.attributes(f.div).size(), std::size_t{2});

  f.document.set_attribute(f.div, "class", "replaced");
  CHECK(f.document.has_class(f.div, "replaced"));
  CHECK(!f.document.has_class(f.div, "card"));
}

BONK_TEST(dom, node_ids_are_generational) {
  Fixture f;
  const NodeId paragraph = f.paragraph;
  CHECK(f.document.is_alive(paragraph));

  f.document.remove_child(f.div, paragraph);
  CHECK(!f.document.is_alive(paragraph));  // stale id is detected, not a crash
  CHECK_EQ(f.document.text_content(f.div), std::string(""));

  // A fresh node can occupy the same index but carries a new generation.
  const NodeId replacement = f.document.create_element("span");
  f.document.append_child(f.div, replacement);
  CHECK(f.document.is_alive(replacement));
  CHECK(replacement != paragraph);
}

BONK_TEST(dom, insert_before_keeps_order) {
  Fixture f;
  const NodeId first = f.document.create_element("span");
  const NodeId second = f.document.create_element("em");
  f.document.insert_before(f.div, first, f.paragraph);
  f.document.insert_before(f.div, second, f.paragraph);
  const std::vector<NodeId> kids = f.document.children(f.div);
  REQUIRE(kids.size() == 3);
  CHECK_EQ(kids[0], first);
  CHECK_EQ(kids[1], second);
  CHECK_EQ(kids[2], f.paragraph);
}

BONK_TEST(dom, serialization_escapes) {
  Fixture f;
  const NodeId text = f.document.create_text("a < b & c > d");
  f.document.append_child(f.paragraph, text);
  const std::string html = f.document.to_html(f.document.root());
  CHECK(html.find("a &lt; b &amp; c &gt; d") != std::string::npos);
  CHECK(html.find("<div class=\"card wide\" id=\"main\">") != std::string::npos);
  CHECK(html.find("</div>") != std::string::npos);
}

BONK_TEST(dom, debug_tree_outline) {
  Fixture f;
  const std::string tree = f.document.debug_tree(f.document.root());
  CHECK_EQ(tree.rfind("#document\n", 0), std::size_t{0});
  CHECK(tree.find("  <html>") != std::string::npos);
  CHECK(tree.find("#text \"hello world\"") != std::string::npos);
}

BONK_TEST(selector, parse_and_match_basics) {
  Fixture f;
  CHECK_EQ(f.document.query_selector(f.document.root(), "p").valid(), true);
  CHECK_EQ(f.document.query_selector(f.document.root(), "#main"), f.div);
  CHECK_EQ(f.document.query_selector(f.document.root(), ".note"), f.paragraph);
  CHECK_EQ(f.document.query_selector(f.document.root(), "div.card"), f.div);
  CHECK(!f.document.query_selector(f.document.root(), "article").valid());
  CHECK_EQ(f.document.query_selector_all(f.document.root(), "div, p").size(), std::size_t{2});
  CHECK(f.document.query_selector(f.document.root(), "body > div").valid());
  CHECK(!f.document.query_selector(f.document.root(), "html > div").valid());
  CHECK(f.document.query_selector(f.document.root(), "body div p").valid());
}

BONK_TEST(selector, attribute_selectors) {
  Fixture f;
  CHECK(f.document.query_selector(f.document.root(), "[id]").valid());
  CHECK(f.document.query_selector(f.document.root(), "[id=\"main\"]").valid());
  CHECK(!f.document.query_selector(f.document.root(), "[id=\"other\"]").valid());
  CHECK(f.document.query_selector(f.document.root(), "[class~=\"wide\"]").valid());
  CHECK(f.document.query_selector(f.document.root(), "[id^=\"ma\"]").valid());
  CHECK(f.document.query_selector(f.document.root(), "[id$=\"in\"]").valid());
  CHECK(f.document.query_selector(f.document.root(), "[id*=\"ai\"]").valid());
}

BONK_TEST(selector, pseudo_classes) {
  Fixture f;
  CHECK_EQ(f.document.query_selector(f.document.root(), "html:root"), f.html);
  CHECK_EQ(f.document.query_selector(f.document.root(), "div:first-child"), f.div);
  CHECK_EQ(f.document.query_selector(f.document.root(), "div:last-child"), f.div);
  CHECK_EQ(f.document.query_selector(f.document.root(), "p:only-child"), f.paragraph);
  CHECK(!f.document.query_selector(f.document.root(), "p:not(.note)").valid());
  CHECK(f.document.query_selector(f.document.root(), "p:not(.other)").valid());
}

BONK_TEST(selector, specificity_is_computed) {
  const Result<std::vector<ComplexSelector>> parsed = parse_selector_list("#a .b c, div, *");
  REQUIRE(parsed.is_ok());
  const std::vector<ComplexSelector>& list = parsed.value();
  REQUIRE(list.size() == 3);
  CHECK_EQ(list[0].specificity.ids, 1);
  CHECK_EQ(list[0].specificity.classes, 1);
  CHECK_EQ(list[0].specificity.types, 1);
  CHECK_EQ(list[1].specificity.types, 1);
  CHECK_EQ(list[2].specificity.packed(), 0ull);
  CHECK(list[0].specificity.packed() > list[1].specificity.packed());
  CHECK_EQ(list[0].text(), std::string("#a .b c"));
}

BONK_TEST(selector, parse_errors) {
  CHECK(parse_selector_list("").is_error());
  CHECK(parse_selector_list("div,").is_error());
  CHECK(parse_selector_list(". ").is_error());
  CHECK(parse_selector_list("[unclosed").is_error());
  CHECK(parse_selector_list("a::before").is_error());
  CHECK(parse_selector_list(":hover").is_ok());  // valid, simply never matches
}

BONK_TEST(selector, ids_and_classes_are_case_sensitive) {
  Fixture f;
  CHECK(!f.document.query_selector(f.document.root(), ".Note").valid());
  CHECK(!f.document.query_selector(f.document.root(), "#Main").valid());
  CHECK(f.document.query_selector(f.document.root(), "P").valid());  // type selectors are not
}

BONK_TEST(dom, unlink_moves_a_subtree_without_killing_it) {
  dom::Document document;
  const dom::NodeId parent = document.create_element("div");
  const dom::NodeId child = document.create_element("span");
  document.append_child(document.root(), parent);
  document.append_child(parent, child);

  document.unlink(child);
  CHECK(document.is_alive(child));
  CHECK_EQ(document.parent(child), dom::NodeId{});
  CHECK_EQ(document.children(parent).size(), std::size_t{0});

  document.append_child(document.root(), child);  // and it can be re-attached
  CHECK_EQ(document.parent(child), document.root());

  document.remove_child(document.root(), child);   // this one does destroy it
  CHECK(!document.is_alive(child));
}

BONK_TEST(dom, fragments_hold_children_and_serialise_them) {
  dom::Document document;
  const dom::NodeId fragment = document.create_fragment();
  CHECK_EQ(document.type(fragment), dom::NodeType::Fragment);
  CHECK_EQ(dom::node_type_name(document.type(fragment)), std::string_view("fragment"));
  document.append_text(fragment, "hello");
  CHECK_EQ(document.text_content(fragment), std::string("hello"));
  CHECK_EQ(document.to_html(fragment), std::string("hello"));
}

BONK_TEST(dom, extend_text_appends_to_an_existing_text_node) {
  dom::Document document;
  const dom::NodeId paragraph = document.create_element("p");
  document.append_child(document.root(), paragraph);
  document.append_text(paragraph, "one");
  const dom::NodeId text = document.first_child(paragraph);
  REQUIRE(text.valid());
  document.extend_text(text, " two");
  CHECK_EQ(document.text_data(text), std::string("one two"));
  CHECK_EQ(document.children(paragraph).size(), std::size_t{1});  // still one node
}
