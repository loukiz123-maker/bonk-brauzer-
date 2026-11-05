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
