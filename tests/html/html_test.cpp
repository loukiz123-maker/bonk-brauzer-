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

BONK_TEST(html, select_parses_ordinary_content) {
  // The relaxed <select> content model: anything but option/optgroup/input is parsed
  // with the "in body" rules, so a <b> inside a select really is an element.
  CHECK_EQ(parse_and_serialize("<select><b><option>x"),
           std::string("<html><head></head><body><select><b><option>x</option></b></select></body></html>"));
  CHECK_EQ(parse_and_serialize("<select><div>d</div></select>"),
           std::string("<html><head></head><body><select><div>d</div></select></body></html>"));
  // A nested <select> closes the outer one and is then dropped, so the second
  // <option> ends up as a stray child of <body> — which is what html5lib expects.
  CHECK_EQ(parse_and_serialize("<select><option>a<select><option>b"),
           std::string("<html><head></head><body><select><option>a</option></select>"
                       "<option>b</option></body></html>"));
}

BONK_TEST(html, select_is_a_scope_boundary_for_the_adoption_agency) {
  // `<select>` ends an "in scope" walk, so `</font>` cannot see the <font> that
  // encloses the select: the tag is ignored instead of pulling the select out.
  CHECK_EQ(parse_and_serialize("<font><select><option>a</option></font></select>"),
           std::string("<html><head></head><body><font><select><option>a</option></select></font>"
                       "</body></html>"));
}

BONK_TEST(html, noahs_ark_compares_attribute_values) {
  // Three <font size=4> are already active, so the fourth one is dropped and the
  // list keeps 4b/4c/<font size=5>/4d — a comparison that only looked at the
  // number of attributes would drop the size=5 entry as well.
  CHECK_EQ(parse_and_serialize("<p><font size=4><font size=4><font size=4><font size=5><font size=4><p>X"),
           std::string("<html><head></head><body>"
                       "<p><font size=\"4\"><font size=\"4\"><font size=\"4\"><font size=\"5\">"
                       "<font size=\"4\"></font></font></font></font></font></p>"
                       "<p><font size=\"4\"><font size=\"4\"><font size=\"5\"><font size=\"4\">X"
                       "</font></font></font></font></p></body></html>"));
}

BONK_TEST(html, table_foster_parenting_reconstructs_formatting) {
  // "Any other start tag" inside "in table" reconstructs the active formatting
  // elements while foster parenting is still on, so the <font> clone and the <img>
  // both land in front of the table.
  CHECK_EQ(parse_and_serialize("<table><center> <font>a</center> <img>"),
           std::string("<html><head></head><body><center> <font>a</font></center>"
                       "<font><img></font><table> </table></body></html>"));
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

BONK_TEST(html, template_adoption_agency_lands_in_the_contents) {
  // The common ancestor of the formatting element is the <template> itself, so the
  // furthest block (the <menu>) has to be moved into the template's contents — not
  // appended to the template element.
  CHECK_EQ(parse_and_serialize("<template><i><menu>Foo</i>"),
           std::string("<html><head><template><i></i><menu><i>Foo</i></menu></template></head>"
                       "<body></body></html>"));
}

BONK_TEST(html, template_mode_is_decided_by_the_first_content_token) {
  // A `<div>` switches the template to "in body", so the following `<tr>` is a
  // table part outside a table and gets dropped ...
  CHECK_EQ(parse_and_serialize("<template><div>Foo</div><tr></tr>"),
           std::string("<html><head><template><div>Foo</div></template></head><body></body></html>"));
  // ... while a `<tr>` first still builds a table inside the contents, and the
  // head-ish tokens (`<script>`, `<meta>`) leave the template's mode alone.
  CHECK_EQ(parse_and_serialize("<template><tr></tr>"),
           std::string("<html><head><template><tr></tr></template></head><body></body></html>"));
  CHECK_EQ(parse_and_serialize("<template><script>x</script><td></td>"),
           std::string("<html><head><template><script>x</script><td></td></template></head>"
                       "<body></body></html>"));
}

BONK_TEST(html, template_ignores_other_end_tags) {
  // "In template" only understands `</template>`: a `</div>` there is a no-op even
  // when a <div> is open inside the contents.
  CHECK_EQ(parse_and_serialize("<template><div>a</div></div>b"),
           std::string("<html><head><template><div>a</div>b</template></head><body></body></html>"));
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
  // Closing the outer button leaves the <b> in the active formatting list, and the
  // new button is inserted after a reconstruction — hence the cloned <b>.
  CHECK_EQ(parse_and_serialize("<button>a<b>b<button>c"),
           std::string("<html><head></head><body><button>a<b>b</b></button><b><button>c</button></b></body></html>"));
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

// --- fragment parsing (Element.innerHTML) -----------------------------------
// `parse_fragment` builds the children of a context element that is *not* on the
// stack of open elements, so nothing in the input can pop it; the tokenizer starts
// in the state the context tag demands.

namespace {

std::string fragment_children(std::string_view input, std::string_view context) {
  dom::Document document;
  const dom::NodeId element = parse_fragment(input, context, document, nullptr, {});
  std::string out;
  for (const dom::NodeId child : document.children(element)) out += document.to_html(child);
  return out;
}

}  // namespace

BONK_TEST(html, fragment_div_parses_ordinary_markup) {
  CHECK_EQ(fragment_children("<p>a<b>b</p>c", "div"), std::string("<p>a<b>b</b></p><b>c</b>"));
}

BONK_TEST(html, fragment_context_element_is_never_popped) {
  // `</div>` has nothing to match: it must not close the context element.
  CHECK_EQ(fragment_children("</div><b>x</b>", "div"), std::string("<b>x</b>"));
}

BONK_TEST(html, fragment_ignores_document_skeleton_tags) {
  CHECK_EQ(fragment_children("<body><head><html><span>x", "body"), std::string("<span>x</span>"));
}

BONK_TEST(html, fragment_table_context_grows_table_parts) {
  // "In table": a row gets its implicit <tbody>.
  CHECK_EQ(fragment_children("<tr><td>x", "table"), std::string("<tbody><tr><td>x</td></tr></tbody>"));
}

BONK_TEST(html, fragment_tr_context_creates_cells) {
  CHECK_EQ(fragment_children("<td>a</td><td>b", "tr"), std::string("<td>a</td><td>b</td>"));
}

BONK_TEST(html, fragment_td_context_uses_the_body_rules) {
  // The spec's "reset the insertion mode" keeps "in cell" for cells that are really
  // on the stack, so a `td` context parses with the "in body" rules: `<tr>`/`<td>`
  // are parse errors and only the text survives.
  CHECK_EQ(fragment_children("<tr><td>a", "td"), std::string("a"));
}

BONK_TEST(html, fragment_colgroup_drops_text) {
  CHECK_EQ(fragment_children("foo<col>", "colgroup"), std::string("<col>"));
}

BONK_TEST(html, fragment_title_context_is_rcdata) {
  CHECK_EQ(fragment_children("a&amp;b<b>c", "title"), std::string("a&amp;b&lt;b&gt;c"));
}

BONK_TEST(html, fragment_text_context_has_no_appropriate_end_tag) {
  // The context tag was never a start tag we emitted, so `</script>` stays text.
  CHECK_EQ(fragment_children("a()</script>b", "script"), std::string("a()&lt;/script&gt;b"));
}

BONK_TEST(html, fragment_plaintext_context_swallows_everything) {
  CHECK_EQ(fragment_children("<b>no</b>", "plaintext"), std::string("&lt;b&gt;no&lt;/b&gt;"));
}

BONK_TEST(html, fragment_select_context_drops_input) {
  CHECK_EQ(fragment_children("<input><option>a", "select"), std::string("<option>a</option>"));
}

BONK_TEST(html, fragment_template_context_keeps_contents_inline) {
  CHECK_EQ(fragment_children("<div>t</div>", "template"), std::string("<div>t</div>"));
}

BONK_TEST(html, fragment_html_context_returns_the_document_element) {
  dom::Document document;
  const dom::NodeId element = parse_fragment("<body>X", "html", document, nullptr, {});
  CHECK(document.type(element) == dom::NodeType::Element);
  CHECK_EQ(document.tag_name(element), std::string_view("html"));
  CHECK_EQ(document.to_html(element), std::string("<html><head></head><body>X</body></html>"));
}

BONK_TEST(html, fragment_context_element_hangs_off_the_document) {
  dom::Document document;
  const dom::NodeId element = parse_fragment("x", "textarea", document, nullptr, {});
  CHECK_EQ(document.tag_name(element), std::string_view("textarea"));
  CHECK_EQ(document.to_html(element), std::string("<textarea>x</textarea>"));
  CHECK_EQ(document.children(document.root()).size(), static_cast<std::size_t>(2));  // <html> + context
}

BONK_TEST(html, comment_after_html_end_tag_goes_to_the_document) {
  // "After after body": a comment past `</html>` is a child of the Document, not of
  // <html> and certainly not of <body>.
  CHECK_EQ(parse_and_serialize("<p>a</p></html><!--x-->"),
           std::string("<html><head></head><body><p>a</p></body></html><!--x-->"));
  CHECK_EQ(parse_and_serialize("</html><!--x-->"),
           std::string("<html><head></head><body></body></html><!--x-->"));
}

BONK_TEST(html, formatting_walk_stops_cloning_after_four_steps) {
  // The inner loop of the adoption agency drops the nodes it cannot re-parent once
  // it is four steps away from the furthest block, which is why the <em> wrapper
  // disappears here but survives in `<b><em><foo><foo><aside></b>`.
  CHECK_EQ(parse_and_serialize("<b><em><foo><foo><foo><aside></b></em>"),
           std::string("<html><head></head><body><b><em><foo><foo><foo></foo></foo></foo></em></b>"
                       "<aside><b></b></aside></body></html>"));
  CHECK_EQ(parse_and_serialize("<b><em><foo><foo><aside></b></em>"),
           std::string("<html><head></head><body><b><em><foo><foo></foo></foo></em></b>"
                       "<em></em><aside><em><b></b></em></aside></body></html>"));
}

BONK_TEST(html, whitespace_after_head_stays_in_the_html_element) {
  // "After head" inserts whitespace instead of dropping it; only "before head" is
  // allowed to throw it away.
  CHECK_EQ(parse_and_serialize("<head></head>\n<body></body>"),
           std::string("<html><head></head>\n<body></body></html>"));
}

BONK_TEST(html, quirks_mode_keeps_a_table_inside_a_paragraph) {
  // No doctype means quirks mode, where `<table>` does not close an open <p>.
  CHECK_EQ(parse_and_serialize("<p><table></table>"),
           std::string("<html><head></head><body><p><table></table></p></body></html>"));
  CHECK_EQ(parse_and_serialize("<!DOCTYPE html><p><table></table>"),
           std::string("<!DOCTYPE html><html><head></head><body><p></p><table></table></body></html>"));
}

BONK_TEST(html, marquee_drops_the_formatting_children_it_enclosed) {
  // <marquee> pushes a marker on the active-formatting list; closing it clears them.
  CHECK_EQ(parse_and_serialize("<marquee><a href=b>x</marquee>y"),
           std::string("<html><head></head><body><marquee><a href=\"b\">x</a></marquee>y</body></html>"));
}

BONK_TEST(html, template_ignores_html_and_body_tags) {
  CHECK_EQ(parse_and_serialize("<template><div></div><body c=d>x</template>"),
           std::string("<html><head><template><div></div>x</template></head><body></body></html>"));
}

BONK_TEST(html, plaintext_in_a_colgroup_is_fostered_out) {
  CHECK_EQ(parse_and_serialize("<table><colgroup><plaintext>x"),
           std::string("<html><head></head><body><plaintext>x</plaintext>"
                       "<table><colgroup></colgroup></table></body></html>"));
}

BONK_TEST(html, frameset_ok_ignores_ordinary_blocks) {
  CHECK_EQ(parse_and_serialize("<div><frameset><frame></frameset>"),
           std::string("<html><head></head><frameset><frame></frameset></html>"));
}

BONK_TEST(html, body_content_blocks_a_frameset) {
  // <pre>/<li>/<br>/… set the frameset-ok flag to "not ok", so the frameset is
  // dropped and the body survives.
  CHECK_EQ(parse_and_serialize("<pre><frameset></frameset>"),
           std::string("<html><head></head><body><pre></pre></body></html>"));
  CHECK_EQ(parse_and_serialize("<br><frameset></frameset>"),
           std::string("<html><head></head><body><br></body></html>"));
  // Ordinary block content does not touch the flag.
  CHECK_EQ(parse_and_serialize("<div><frameset></frameset>"),
           std::string("<html><head></head><frameset></frameset></html>"));
}

BONK_TEST(html, image_is_renamed_to_img) {
  CHECK_EQ(parse_and_serialize("<image src=x>"),
           std::string("<html><head></head><body><img src=\"x\"></body></html>"));
}

BONK_TEST(html, pre_swallows_one_newline) {
  CHECK_EQ(parse_and_serialize("<pre>\n\ntext</pre>"),
           std::string("<html><head></head><body><pre>\ntext</pre></body></html>"));
  CHECK_EQ(parse_and_serialize("<listing>\ntext</listing>"),
           std::string("<html><head></head><body><listing>text</listing></body></html>"));
}

BONK_TEST(html, noscript_in_head_keeps_only_head_elements) {
  // Scripting is off, so `<noscript>` in <head> is a container: <basefont> and
  // comments stay inside it, while a <p> closes it and lands in the body.
  CHECK_EQ(parse_and_serialize("<head><noscript><basefont><!--x--></noscript>"),
           std::string("<html><head><noscript><basefont><!--x--></noscript></head><body></body></html>"));
  CHECK_EQ(parse_and_serialize("<head><noscript><p>x"),
           std::string("<html><head><noscript></noscript></head><body><p>x</p></body></html>"));
}

BONK_TEST(html, noscript_is_raw_text_with_scripting) {
  ParseOptions options;
  options.scripting_enabled = true;
  dom::Document document;
  CHECK(parse_html("<head><noscript><p>x</noscript>", document, nullptr, options).is_ok());
  CHECK_EQ(document.to_html(document.root()),
           std::string("<html><head><noscript>&lt;p&gt;x</noscript></head><body></body></html>"));
}

BONK_TEST(html, plaintext_inside_a_table_is_fostered_and_swallows_the_rest) {
  // `to_html` escapes text everywhere, so the swallowed markup shows up escaped.
  CHECK_EQ(parse_and_serialize("<table><plaintext>a</table>b"),
           std::string("<html><head></head><body><plaintext>a&lt;/table&gt;b</plaintext><table></table></body></html>"));
}

BONK_TEST(html, hidden_input_stays_inside_a_table) {
  // `type=hidDEN` is a case-insensitive match for "hidden": no foster parenting.
  CHECK_EQ(parse_and_serialize("<table><input type=hidDEN></table>"),
           std::string("<html><head></head><body><table><input type=\"hidDEN\"></table></body></html>"));
}

BONK_TEST(html, second_form_is_ignored) {
  CHECK_EQ(parse_and_serialize("<form><form><input></form>"),
           std::string("<html><head></head><body><form><input></form></body></html>"));
}

BONK_TEST(html, form_pointer_outlives_the_stack) {
  // `<form>` inside `<table>` is inserted and popped right away, but the form
  // element pointer keeps it open: `</form>` clears the pointer (finding no form
  // in scope) and a following `<form>` is therefore a *new* form.
  CHECK_EQ(parse_and_serialize("<form><table></form><form></table></form>"),
           std::string("<html><head></head><body><form><table><form></form></table></form>"
                       "</body></html>"));
  // `</form>` removes the form element from the stack without closing its
  // children, which is the documented oddity of the pointer-based rule.
  CHECK_EQ(parse_and_serialize("<form><div></form>x"),
           std::string("<html><head></head><body><form><div>x</div></form></body></html>"));
}

BONK_TEST(html, select_in_table_closes_on_table_tokens) {
  CHECK_EQ(parse_and_serialize("<table><select><option>A<tr><td>B</td></tr></table>"),
           std::string("<html><head></head><body><select><option>A</option></select><table><tbody><tr><td>B</td>"
                       "</tr></tbody></table></body></html>"));
}
