#include "test_framework.hpp"

#include "bonk/html/html.hpp"
#include "bonk/html/tokenizer.hpp"

using namespace bonk;
using namespace bonk::html;

namespace {

std::vector<Token> tokenize_all(std::string_view input) {
  Tokenizer tokenizer(input);
  std::vector<Token> tokens;
  for (;;) {
    Token token = tokenizer.next();
    if (token.type == TokenType::EndOfFile) break;
    tokens.push_back(std::move(token));
  }
  return tokens;
}

std::string parse_and_serialize(std::string_view html) {
  Diagnostics diagnostics;
  dom::Document document;
  CHECK(parse_html(html, document, &diagnostics).is_ok());
  return document.to_html(document.root());
}

}  // namespace

BONK_TEST(tokenizer, start_tag_with_attributes) {
  const std::vector<Token> tokens = tokenize_all("<div class=\"a b\" DATA-X=1 hidden>");
  REQUIRE(!tokens.empty());
  CHECK_EQ(tokens[0].type, TokenType::StartTag);
  CHECK_EQ(tokens[0].name, std::string("div"));
  REQUIRE(tokens[0].attributes.size() == 3);
  CHECK_EQ(tokens[0].attributes[0].first, std::string("class"));
  CHECK_EQ(tokens[0].attributes[0].second, std::string("a b"));
  CHECK_EQ(tokens[0].attributes[1].first, std::string("data-x"));  // names are lowercased
  CHECK_EQ(tokens[0].attributes[1].second, std::string("1"));
  CHECK_EQ(tokens[0].attributes[2].first, std::string("hidden"));
  CHECK_EQ(tokens[0].attributes[2].second, std::string(""));
}

BONK_TEST(tokenizer, u_tag_names_are_lowercased_and_end_tags_recognised) {
  const std::vector<Token> tokens = tokenize_all("<DIV>x</DiV>");
  REQUIRE(tokens.size() == 3);
  CHECK_EQ(tokens[0].name, std::string("div"));
  CHECK_EQ(tokens[1].type, TokenType::Characters);
  CHECK_EQ(tokens[1].data, std::string("x"));
  CHECK_EQ(tokens[2].type, TokenType::EndTag);
  CHECK_EQ(tokens[2].name, std::string("div"));
}

BONK_TEST(tokenizer, self_closing_and_void) {
  const std::vector<Token> tokens = tokenize_all("<br/><img src=x>");
  REQUIRE(tokens.size() == 2);
  CHECK(tokens[0].self_closing);
  CHECK_EQ(tokens[1].name, std::string("img"));
  CHECK_EQ(tokens[1].attribute("src"), std::string_view("x"));
}

BONK_TEST(tokenizer, character_references) {
  const std::vector<Token> tokens = tokenize_all("a&amp;b &lt; c &#65; &#x42; &nbsp;x &unknown;y");
  REQUIRE(!tokens.empty());
  const std::string& text = tokens[0].data;
  CHECK_EQ(text, std::string("a&b < c A B \xC2\xA0x &unknown;y"));
}

BONK_TEST(tokenizer, comments_and_doctype) {
  const std::vector<Token> tokens = tokenize_all("<!DOCTYPE html><!-- hi --><p>");
  REQUIRE(tokens.size() == 3);
  CHECK_EQ(tokens[0].type, TokenType::Doctype);
  CHECK_EQ(tokens[0].data, std::string("html"));
  CHECK_EQ(tokens[1].type, TokenType::Comment);
  CHECK_EQ(tokens[1].data, std::string(" hi "));
  CHECK_EQ(tokens[2].type, TokenType::StartTag);
}

BONK_TEST(tokenizer, rawtext_respects_case_insensitive_end_tag) {
  Tokenizer tokenizer("<style>a > b { color: red }</STYLE><p>x</p>");
  const Token style_start = tokenizer.next();
  REQUIRE(style_start.type == TokenType::StartTag);
  tokenizer.enter_text_element(style_start.name, false);
  const Token text = tokenizer.next();
  CHECK_EQ(text.type, TokenType::Characters);
  CHECK_EQ(text.data, std::string("a > b { color: red }"));
  const Token style_end = tokenizer.next();
  CHECK_EQ(style_end.type, TokenType::EndTag);
  CHECK_EQ(style_end.name, std::string("style"));
}

BONK_TEST(tokenizer, entities_are_decoded_in_rcdata_but_not_rawtext) {
  Tokenizer rcdata("<title>a &amp; b</title>");
  Token start_tag = rcdata.next();
  rcdata.enter_text_element(start_tag.name, true);
  const Token decoded = rcdata.next();
  CHECK_EQ(decoded.data, std::string("a & b"));

  Tokenizer rawtext("<script>a &amp; b</script>");
  Token script_tag = rawtext.next();
  rawtext.enter_text_element(script_tag.name, false);
  const Token raw = rawtext.next();
  CHECK_EQ(raw.data, std::string("a &amp; b"));
}

BONK_TEST(tokenizer, newlines_are_normalised) {
  const std::vector<Token> tokens = tokenize_all("a\r\nb\rc");
  REQUIRE(!tokens.empty());
  CHECK_EQ(tokens[0].data, std::string("a\nb\nc"));
}

// A leading BOM is removed by the document loader, not by the tokenizer: the
// tokenizer must hand through exactly the bytes it is given (html5lib corpus).
BONK_TEST(tokenizer, bom_is_passed_through) {
  const std::vector<Token> tokens = tokenize_all("\xEF\xBB\xBF" "a");
  REQUIRE(!tokens.empty());
  CHECK_EQ(tokens[0].data, std::string("\xEF\xBB\xBF" "a"));
}

BONK_TEST(html, leading_bom_is_stripped_by_the_parser) {
  const std::string html = parse_and_serialize("\xEF\xBB\xBF" "<p>hi</p>");
  CHECK_EQ(html, std::string("<html><head></head><body><p>hi</p></body></html>"));
}

BONK_TEST(html, implicit_html_head_body) {
  const std::string html = parse_and_serialize("<p>hello</p>");
  CHECK_EQ(html, std::string("<html><head></head><body><p>hello</p></body></html>"));
}

BONK_TEST(html, head_elements_go_to_head) {
  const std::string html = parse_and_serialize("<title>T</title><style>p{}</style><p>x</p>");
  CHECK_EQ(html, std::string("<html><head><title>T</title><style>p{}</style></head><body><p>x</p></body></html>"));
}

BONK_TEST(html, void_elements_and_self_closing) {
  const std::string html = parse_and_serialize("<div><br><img src=\"a.png\"></div>");
  CHECK_EQ(html, std::string("<html><head></head><body><div><br><img src=\"a.png\"></div></body></html>"));
}

BONK_TEST(html, implied_end_tags) {
  const std::string html = parse_and_serialize("<p>one<p>two");
  CHECK_EQ(html, std::string("<html><head></head><body><p>one</p><p>two</p></body></html>"));
}

BONK_TEST(html, nested_lists) {
  const std::string html = parse_and_serialize("<ul><li>a<li>b</ul>");
  CHECK_EQ(html, std::string("<html><head></head><body><ul><li>a</li><li>b</li></ul></body></html>"));
}

BONK_TEST(html, misnested_end_tag_is_reported_not_fatal) {
  Diagnostics diagnostics;
  dom::Document document;
  CHECK(parse_html("<div><span>x</div>", document, &diagnostics).is_ok());
  CHECK(diagnostics.size() > 0);
  const std::string html = document.to_html(document.root());
  CHECK(html.find("<span>") != std::string::npos);
}

BONK_TEST(html, doctype_and_comments_survive) {
  const std::string html = parse_and_serialize("<!DOCTYPE html><!--c--><p>x</p>");
  CHECK_EQ(html, std::string("<!DOCTYPE html><!--c--><html><head></head><body><p>x</p></body></html>"));
}

BONK_TEST(html, entities_in_text_and_attributes) {
  const std::string html = parse_and_serialize("<p title=\"a &amp; b\">1 &lt; 2 &amp;&amp; 3 &gt; 2</p>");
  CHECK(html.find("title=\"a &amp; b\"") != std::string::npos);
  CHECK(html.find("1 &lt; 2 &amp;&amp; 3 &gt; 2") != std::string::npos);
}

BONK_TEST(html, cyrillic_and_utf8_pass_through) {
  const std::string html = parse_and_serialize("<p>Привет, мир</p>");
  CHECK(html.find("Привет, мир") != std::string::npos);
}

BONK_TEST(html, parse_document_helper) {
  Diagnostics diagnostics;
  const Result<dom::Document> document = parse_document("<h1>Hi</h1>", "test.html", &diagnostics);
  REQUIRE(document.is_ok());
  const dom::NodeId heading = document.value().query_selector(document.value().root(), "h1");
  REQUIRE(heading.valid());
  CHECK_EQ(document.value().text_content(heading), std::string("Hi"));
  CHECK_EQ(document.value().source_url(), std::string("test.html"));
}

// --- adoption agency, ruby annotations, "in select" --------------------------
// These four shapes are the canonical misnesting cases from the html5lib tree
// suite (adoption01/adoption02). Before the adoption agency algorithm they either
// crashed the builder or produced a <b>/<a> that swallowed the rest of the page.

BONK_TEST(html, adoption_agency_reopens_closed_formatting) {
  CHECK_EQ(parse_and_serialize("<p><b>1<i>2</b>3</i>4"),
           std::string("<html><head></head><body><p><b>1<i>2</i></b><i>3</i>4</p></body></html>"));
}

BONK_TEST(html, adoption_agency_uses_the_furthest_block) {
  CHECK_EQ(parse_and_serialize("<b><p></b></p>"),
           std::string("<html><head></head><body><b></b><p><b></b></p></body></html>"));
}

BONK_TEST(html, adoption_agency_for_anchors) {
  CHECK_EQ(parse_and_serialize("<a>1<p>2</a>3</p>"),
           std::string("<html><head></head><body><a>1</a><p><a>2</a>3</p></body></html>"));
  CHECK_EQ(parse_and_serialize("<a><p></a></p>"),
           std::string("<html><head></head><body><a></a><p><a></a></p></body></html>"));
}

BONK_TEST(html, formatting_elements_are_reconstructed_after_a_block) {
  CHECK_EQ(parse_and_serialize("<p><b>x<div>y"),
           std::string("<html><head></head><body><p><b>x</b></p><div><b>y</b></div></body></html>"));
}

BONK_TEST(html, ruby_annotations_become_siblings) {
  CHECK_EQ(parse_and_serialize("<ruby>a<rb>b<rb>c"),
           std::string("<html><head></head><body><ruby>a<rb>b</rb><rb>c</rb></ruby></body></html>"));
  CHECK_EQ(parse_and_serialize("<ruby>a<rtc>b<rt>c"),
           std::string("<html><head></head><body><ruby>a<rtc>b<rt>c</rt></rtc></ruby></body></html>"));
}

BONK_TEST(html, select_ignores_foreign_tags) {
  CHECK_EQ(parse_and_serialize("<select><b><option>x"),
           std::string("<html><head></head><body><select><option>x</option></select></body></html>"));
  // A nested <select> closes the outer one and is then dropped, so the second
  // <option> ends up as a stray child of <body> — which is what html5lib expects.
  CHECK_EQ(parse_and_serialize("<select><option>a<select><option>b"),
           std::string("<html><head></head><body><select><option>a</option></select>"
                       "<option>b</option></body></html>"));
}

// --- <template> and <frameset> ----------------------------------------------
// `<template>` keeps its children in a separate DocumentFragment ("template
// contents"); `<frameset>` replaces an empty body instead of becoming a child.

BONK_TEST(html, template_contents_live_in_a_fragment) {
  CHECK_EQ(parse_and_serialize("<template><div>x</div></template>"),
           std::string("<html><head><template><div>x</div></template></head><body></body></html>"));

  dom::Document document;
  CHECK(parse_html("<template><div>x</div></template>", document, nullptr).is_ok());
  const dom::NodeId templ = document.query_selector(document.root(), "template");
  REQUIRE(templ.valid());
  const std::vector<dom::NodeId> children = document.children(templ);
  REQUIRE(children.size() == 1);
  CHECK_EQ(document.type(children[0]), dom::NodeType::Fragment);  // not an element child
  const std::vector<dom::NodeId> content = document.children(children[0]);
  REQUIRE(content.size() == 1);
  CHECK_EQ(document.tag_name(content[0]), std::string("div"));
}

BONK_TEST(html, template_in_body_and_inside_a_table) {
  CHECK_EQ(parse_and_serialize("<body><template><p>a</p></template><p>b"),
           std::string("<html><head></head><body><template><p>a</p></template><p>b</p></body></html>"));
  // A <td> inside <template> belongs to the template, so no implicit <tbody>/<tr>.
  CHECK_EQ(parse_and_serialize("<table><template><td>"),
           std::string("<html><head></head><body><table><template><td></td></template></table></body></html>"));
  // </div> cannot escape the template: it is a scope boundary.
  CHECK_EQ(parse_and_serialize("<div><template></div>Hello"),
           std::string("<html><head></head><body><div><template>Hello</template></div></body></html>"));
}

BONK_TEST(html, frameset_replaces_an_empty_body) {
  CHECK_EQ(parse_and_serialize("<p><frameset><frame></frameset>"),
           std::string("<html><head></head><frameset><frame></frameset></html>"));
  CHECK_EQ(parse_and_serialize("<frameset><frame src=a><noframes>x</noframes></frameset>"),
           std::string("<html><head></head><frameset><frame src=\"a\"><noframes>x</noframes></frameset></html>"));
}

BONK_TEST(html, frameset_is_ignored_once_the_body_has_content) {
  CHECK_EQ(parse_and_serialize("<b>x<frameset><frame>"),
           std::string("<html><head></head><body><b>x</b></body></html>"));
}

BONK_TEST(html, nested_lists_nest) {
  CHECK_EQ(parse_and_serialize("<ul><li>a<ul><li>b"),
           std::string("<html><head></head><body><ul><li>a<ul><li>b</li></ul></li></ul></body></html>"));
}

// --- insertion modes: tables -------------------------------------------------
// The parser dispatches on the current node: <table> → Table, <tbody> → TableBody,
// <tr> → Row, <td> → Cell. Everything else inside a table is foster-parented.

BONK_TEST(html, table_insertion_modes_build_the_implicit_parts) {
  CHECK_EQ(parse_and_serialize("<table><td>x"),
           std::string("<html><head></head><body><table><tbody><tr><td>x</td></tr></tbody></table></body></html>"));
  CHECK_EQ(parse_and_serialize("<table><tr><td>a<td>b</tr></table>"),
           std::string("<html><head></head><body><table><tbody><tr><td>a</td><td>b</td></tr></tbody></table>"
                       "</body></html>"));
  CHECK_EQ(parse_and_serialize("<table><caption>c</caption><tr><td>x"),
           std::string("<html><head></head><body><table><caption>c</caption><tbody><tr><td>x</td></tr>"
                       "</tbody></table></body></html>"));
}

BONK_TEST(html, table_foster_parents_foreign_content) {
  // Text and elements that cannot live in a table are moved in front of it.
  CHECK_EQ(parse_and_serialize("<table>x<td>y"),
           std::string("<html><head></head><body>x<table><tbody><tr><td>y</td></tr></tbody></table></body></html>"));
  CHECK_EQ(parse_and_serialize("<table><div>x</div></table>"),
           std::string("<html><head></head><body><div>x</div><table></table></body></html>"));
}

BONK_TEST(html, table_cells_close_each_other) {
  CHECK_EQ(parse_and_serialize("<table><tr><td><b>a</b></table>"),
           std::string("<html><head></head><body><table><tbody><tr><td><b>a</b></td></tr></tbody></table>"
                       "</body></html>"));
}

BONK_TEST(html, end_tags_before_body_are_ignored) {
  CHECK_EQ(parse_and_serialize("<html></p><!--foo-->"),
           std::string("<html><!--foo--><head></head><body></body></html>"));
  CHECK_EQ(parse_and_serialize("<head></head></p><!--foo-->"),
           std::string("<html><head></head><!--foo--><body></body></html>"));
}

// --- foster parenting, precisely --------------------------------------------
// Foster parenting only applies when the insertion target is a table part, text
// joins the node already in front of the table, and formatting elements keep
// being reconstructed while the parser is still in a table mode.

BONK_TEST(html, fostered_text_joins_one_node) {
  CHECK_EQ(parse_and_serialize("<table>A<td>B</td>C</table>"),
           std::string("<html><head></head><body>AC<table><tbody><tr><td>B</td></tr></tbody></table>"
                       "</body></html>"));
}

BONK_TEST(html, fostered_children_stay_inside_their_element) {
  CHECK_EQ(parse_and_serialize("<table><div>x</div></table>"),
           std::string("<html><head></head><body><div>x</div><table></table></body></html>"));
}

BONK_TEST(html, table_mode_reconstructs_formatting_elements) {
  CHECK_EQ(parse_and_serialize("<table><a>1<td>2</td>3</table>"),
           std::string("<html><head></head><body><a>1</a><a>3</a><table><tbody><tr><td>2</td></tr></tbody>"
                       "</table></body></html>"));
}

BONK_TEST(html, template_switches_to_table_modes) {
  // The template insertion mode becomes "in table body" after <tr>, so the next
  // <td> gets an implicit <tr> of its own — inside the template contents.
  CHECK_EQ(parse_and_serialize("<template><tr></tr><td></td></template>"),
           std::string("<html><head><template><tr></tr><tr><td></td></tr></template></head><body></body></html>"));
}

// --- "in body" rules and table scope ----------------------------------------
// These rules decide where a token is *allowed* to go: a row/cell/section is only
// closed when it is in table scope, a heading is popped one at a time, and a
// nested <button> closes the outer one.

BONK_TEST(html, closing_a_table_returns_to_the_body) {
  CHECK_EQ(parse_and_serialize("<table><td>A</table>B"),
           std::string("<html><head></head><body><table><tbody><tr><td>A</td></tr></tbody></table>B</body></html>"));
}

BONK_TEST(html, nested_button_closes_the_outer_one) {
  CHECK_EQ(parse_and_serialize("<button>a<b>b<button>c"),
           std::string("<html><head></head><body><button>a<b>b</b></button><button><b>c</b></button></body></html>"));
}

BONK_TEST(html, headings_close_one_at_a_time) {
  CHECK_EQ(parse_and_serialize("<h1>one<h2>two"),
           std::string("<html><head></head><body><h1>one</h1><h2>two</h2></body></html>"));
  // A <div> in between stops the implicit close: the spec pops only a heading that
  // is the current node.
  CHECK_EQ(parse_and_serialize("<h1>one<div>two<h2>three"),
           std::string("<html><head></head><body><h1>one<div>two<h2>three</h2></div></h1></body></html>"));
}

BONK_TEST(html, nul_characters_are_dropped_in_body) {
  const std::string input("<p>a\0a</p>", 11);
  CHECK_EQ(parse_and_serialize(input),
           std::string("<html><head></head><body><p>aa</p></body></html>"));
}

BONK_TEST(html, search_closes_a_paragraph) {
  CHECK_EQ(parse_and_serialize("<p>foo<search>bar<p>baz"),
           std::string("<html><head></head><body><p>foo</p><search>bar<p>baz</p></search></body></html>"));
}
