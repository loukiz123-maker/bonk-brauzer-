#include "test_framework.hpp"

#include "bonk/css/css.hpp"
#include "bonk/html/html.hpp"

using namespace bonk;
using namespace bonk::css;

namespace {

struct StyledPage {
  dom::Document document;
  StyleSheet sheet;
  StyleMap styles;

  StyledPage(std::string_view html, std::string_view css, float width = 1000.0f) {
    Diagnostics diagnostics;
    CHECK(html::parse_html(html, document, &diagnostics).is_ok());
    Result<StyleSheet> parsed = StyleSheet::parse(css, &diagnostics);
    CHECK(parsed.is_ok());
    sheet = std::move(parsed.value());
    styles = compute_styles(document, sheet, width, &diagnostics);
  }

  const ComputedStyle& style_of(std::string_view selector) {
    const dom::NodeId node = document.query_selector(document.root(), selector);
    CHECK(node.valid());
    return styles.get(document, node);
  }
};

}  // namespace

BONK_TEST(css, parse_simple_rule) {
  Result<StyleSheet> sheet = StyleSheet::parse("p { color: red; margin: 4px 8px }");
  REQUIRE(sheet.is_ok());
  REQUIRE(sheet.value().rules().size() == 1);
  const Rule& rule = sheet.value().rules()[0];
  CHECK_EQ(rule.selectors.size(), std::size_t{1});
  CHECK_EQ(rule.declarations.size(), std::size_t{2});
  CHECK_EQ(rule.declarations[0].property, std::string("color"));
  CHECK_EQ(rule.declarations[0].value, std::string("red"));
}

BONK_TEST(css, comments_are_stripped) {
  Result<StyleSheet> sheet = StyleSheet::parse("/* c1 */ p { /* inner */ color: blue; } /* tail");
  REQUIRE(sheet.is_ok());
  REQUIRE(sheet.value().rules().size() == 1);
  CHECK_EQ(sheet.value().rules()[0].declarations.size(), std::size_t{1});
}

BONK_TEST(css, important_flag) {
  Result<StyleSheet> sheet = StyleSheet::parse("p { color: red !important; background: blue }");
  REQUIRE(sheet.is_ok());
  const Rule& rule = sheet.value().rules()[0];
  CHECK(rule.declarations[0].important);
  CHECK_EQ(rule.declarations[0].value, std::string("red"));
  CHECK(!rule.declarations[1].important);
}

BONK_TEST(css, media_queries) {
  Result<StyleSheet> sheet = StyleSheet::parse(
      "@media screen { p { color: red } } @media print { p { color: black } } "
      "@media (min-width: 800px) { p { font-size: 20px } }");
  REQUIRE(sheet.is_ok());
  REQUIRE(sheet.value().rules().size() == 3);
  CHECK(sheet.value().rules()[0].media.matches(1000.0f));
  CHECK(!sheet.value().rules()[1].media.matches(1000.0f));
  CHECK(sheet.value().rules()[2].media.matches(1000.0f));
  CHECK(!sheet.value().rules()[2].media.matches(500.0f));
}

BONK_TEST(css, invalid_selector_is_reported_and_skipped) {
  Diagnostics diagnostics;
  Result<StyleSheet> sheet = StyleSheet::parse("p { color: red } a::b { color: blue } div { color: lime }",
                                               &diagnostics);
  REQUIRE(sheet.is_ok());
  CHECK_EQ(sheet.value().rules().size(), std::size_t{2});
  CHECK(diagnostics.size() >= 1);
}

BONK_TEST(css, color_parsing) {
  Color color;
  CHECK(parse_color("#f00", color));
  CHECK_EQ(static_cast<int>(color.r), 255);
  CHECK_EQ(static_cast<int>(color.a), 255);
  CHECK(parse_color("#102030", color));
  CHECK_EQ(static_cast<int>(color.g), 0x20);
  CHECK(parse_color("#10203040", color));
  CHECK_EQ(static_cast<int>(color.a), 0x40);
  CHECK(parse_color("rgb(1, 2, 3)", color));
  CHECK_EQ(static_cast<int>(color.b), 3);
  CHECK(parse_color("rgba(10, 20, 30, 0.5)", color));
  CHECK_EQ(static_cast<int>(color.a), 128);
  CHECK(parse_color("RebeccaPurple", color) == false);
  CHECK(parse_color("transparent", color));
  CHECK(color.is_transparent());
  CHECK(parse_color("nonsense", color) == false);
  CHECK(parse_color("#12345", color) == false);
}

BONK_TEST(css, length_parsing) {
  Length length;
  CHECK(parse_length("12px", length));
  CHECK_EQ(length.unit, Length::Unit::Px);
  CHECK(parse_length("1.5em", length));
  CHECK_EQ(length.unit, Length::Unit::Em);
  CHECK(parse_length("50%", length));
  CHECK_EQ(length.unit, Length::Unit::Percent);
  CHECK(parse_length("auto", length));
  CHECK(length.is_auto());
  CHECK(parse_length("2rem", length));
  CHECK_EQ(length.unit, Length::Unit::Rem);
  CHECK(parse_length("12pt", length));
  CHECK_EQ(length.unit, Length::Unit::Pt);
  CHECK(parse_length("bogus", length) == false);
  CHECK(parse_length("12furlong", length) == false);

  CHECK_EQ(absolute_length_to_px(Length::px(7.0f)), 7.0f);
  CHECK_EQ(static_cast<int>(absolute_length_to_px(Length{1.0f, Length::Unit::In})), 96);
  CHECK_EQ(static_cast<int>(absolute_length_to_px(Length{12.0f, Length::Unit::Pt})), 16);
}

BONK_TEST(css, cascade_specificity_and_order) {
  StyledPage page("<p id=\"x\" class=\"c\">hi</p>",
                  "p { color: #111111 } .c { color: #222222 } #x { color: #333333 } p { color: #444444 }");
  const ComputedStyle& style = page.style_of("p");
  CHECK_EQ(static_cast<int>(style.color.r), 0x33);  // id wins
}

BONK_TEST(css, important_beats_specificity) {
  StyledPage page("<p id=\"x\">hi</p>", "p { color: #00ff00 !important } #x { color: #ff0000 }");
  const ComputedStyle& style = page.style_of("p");
  CHECK_EQ(static_cast<int>(style.color.g), 255);
  CHECK_EQ(static_cast<int>(style.color.r), 0);
}

BONK_TEST(css, inheritance_and_relative_font_sizes) {
  StyledPage page("<div><p>text</p></div>", "div { font-size: 20px; color: #123456 } p { font-size: 150% }");
  const ComputedStyle& paragraph = page.style_of("p");
  CHECK_EQ(static_cast<int>(paragraph.font_size), 30);
  CHECK_EQ(static_cast<int>(paragraph.color.g), 0x34);  // inherited colour
}

BONK_TEST(css, em_based_margins) {
  StyledPage page("<p>text</p>", "p { font-size: 10px; margin: 2em }");
  const ComputedStyle& style = page.style_of("p");
  CHECK_EQ(static_cast<int>(style.margin[0].value), 20);
  CHECK_EQ(style.margin[0].unit, Length::Unit::Px);
}

BONK_TEST(css, box_shorthands) {
  StyledPage page("<div><p>x</p></div>", "p { margin: 1px 2px 3px 4px; padding: 5px }");
  const ComputedStyle& style = page.style_of("p");
  CHECK_EQ(static_cast<int>(style.margin[1].value), 2);
  CHECK_EQ(static_cast<int>(style.margin[2].value), 3);
  CHECK_EQ(static_cast<int>(style.margin[3].value), 4);
  CHECK_EQ(static_cast<int>(style.padding[0].value), 5);
  CHECK_EQ(static_cast<int>(style.padding[3].value), 5);
}

BONK_TEST(css, border_shorthand) {
  StyledPage page("<div>x</div>", "div { border: 2px solid #ff0000 }");
  const ComputedStyle& style = page.style_of("div");
  for (int i = 0; i < 4; ++i) CHECK_EQ(static_cast<int>(style.border_width[i]), 2);
  CHECK_EQ(static_cast<int>(style.border_color.r), 255);
  CHECK_EQ(static_cast<int>(style.border_color.a), 255);
}

BONK_TEST(css, display_and_text_properties) {
  StyledPage page("<div><span>x</span></div>",
                  "div { display: block; text-align: center; background-color: #abcdef } "
                  "span { display: inline; text-decoration: underline }");
  const ComputedStyle& div = page.style_of("div");
  CHECK_EQ(div.display, Display::Block);
  CHECK_EQ(div.text_align, TextAlign::Center);
  CHECK_EQ(static_cast<int>(div.background_color.g), 0xcd);

  const ComputedStyle& span = page.style_of("span");
  CHECK_EQ(span.display, Display::Inline);
  CHECK_EQ(span.text_decoration, TextDecoration::Underline);
}

BONK_TEST(css, user_agent_styles) {
  StyledPage page("<h1>Title</h1><p>text</p><ul><li>x</li></ul>", "");
  CHECK_EQ(page.style_of("h1").display, Display::Block);
  CHECK_EQ(page.style_of("h1").font_weight, 700);
  CHECK(page.style_of("h1").font_size > 20.0f);  // 2em of 16px
  CHECK_EQ(page.style_of("p").display, Display::Block);
  CHECK_EQ(page.style_of("li").display, Display::ListItem);
  CHECK_EQ(page.style_of("h1").margin[0].unit, Length::Unit::Px);  // em resolved
}

BONK_TEST(css, author_rules_beat_user_agent) {
  StyledPage page("<h1>x</h1>", "h1 { display: inline; font-size: 12px }");
  CHECK_EQ(page.style_of("h1").display, Display::Inline);
  CHECK_EQ(static_cast<int>(page.style_of("h1").font_size), 12);
}

BONK_TEST(css, invisible_elements) {
  StyledPage page("<div><span id=\"hidden\">x</span></div>", "#hidden { display: none }");
  CHECK_EQ(page.style_of("#hidden").display, Display::None);
}

BONK_TEST(css, unsupported_properties_are_ignored) {
  StyledPage page("<div>x</div>", "div { -webkit-fancy: 1; will-change: transform; color: #010203 }");
  CHECK_EQ(static_cast<int>(page.style_of("div").color.b), 0x03);
}

BONK_TEST(css, font_face_is_collected) {
  Result<StyleSheet> sheet = StyleSheet::parse("@font-face { font-family: \"Custom\"; src: url(custom.ttf) }");
  REQUIRE(sheet.is_ok());
  REQUIRE(sheet.value().font_faces().size() == 1);
  CHECK_EQ(sheet.value().font_faces()[0].family, std::string("Custom"));
  CHECK_EQ(sheet.value().font_faces()[0].source, std::string("url(custom.ttf)"));
}

BONK_TEST(css, foreign_type_selectors_keep_their_case) {
  // `svg` and `foreignObject` are matched exactly; the HTML content *inside*
  // `foreignObject` is matched case-insensitively like any other HTML element.
  StyledPage page("<body><svg><foreignObject><span>hi</span></foreignObject></svg></body>",
                  "svg { color: #111111 } foreignObject { color: #222222 } SPAN { color: #333333 }");
  const dom::NodeId svg = page.document.query_selector(page.document.root(), "svg");
  const dom::NodeId foreign_object = page.document.query_selector(page.document.root(), "foreignObject");
  const dom::NodeId span = page.document.query_selector(page.document.root(), "span");
  REQUIRE(svg.valid() && foreign_object.valid() && span.valid());
  CHECK_EQ(static_cast<int>(page.styles.get(page.document, svg).color.r), 0x11);
  CHECK_EQ(static_cast<int>(page.styles.get(page.document, foreign_object).color.r), 0x22);
  CHECK_EQ(static_cast<int>(page.styles.get(page.document, span).color.r), 0x33);
}

BONK_TEST(css, position_insets_and_z_index_are_parsed) {
  StyledPage page("<body><div id=a></div><div id=b></div></body>",
                  "#a { position: absolute; top: 10px; right: 5%; bottom: auto; left: -4px; z-index: 3 }"
                  "#b { position: relative }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  const dom::NodeId b = page.document.query_selector(page.document.root(), "#b");
  REQUIRE(a.valid() && b.valid());
  const css::ComputedStyle& style = page.styles.get(page.document, a);
  CHECK(style.position == css::Position::Absolute);
  CHECK(style.is_positioned());
  CHECK(style.is_out_of_flow());
  CHECK_EQ(static_cast<int>(style.inset[0].value), 10);  // top
  CHECK_EQ(static_cast<int>(style.inset[1].value), 5);   // right, percent
  CHECK(style.inset[1].unit == css::Length::Unit::Percent);
  CHECK(style.inset[2].is_auto());  // bottom: auto
  CHECK_EQ(static_cast<int>(style.inset[3].value), -4);  // left
  CHECK(style.has_z_index);
  CHECK_EQ(style.z_index, 3);

  const css::ComputedStyle& relative = page.styles.get(page.document, b);
  CHECK(relative.position == css::Position::Relative);
  CHECK(!relative.is_out_of_flow());
  CHECK(!relative.has_z_index);
  CHECK(relative.inset[0].is_auto());  // initial value of an inset is `auto`, not 0
}

BONK_TEST(css, unsupported_position_keyword_keeps_the_previous_value) {
  // `sticky` is not modelled; a later `position: sticky` must not silently turn a
  // positioned element back into a static one.
  StyledPage page("<body><div id=a></div></body>", "#a { position: relative } #a { position: sticky }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  REQUIRE(a.valid());
  CHECK(page.styles.get(page.document, a).position == css::Position::Relative);
}

BONK_TEST(css, z_index_auto_resets_a_previous_value) {
  StyledPage page("<body><div id=a></div></body>", "#a { position: absolute; z-index: 7 } #a { z-index: auto }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  REQUIRE(a.valid());
  CHECK(!page.styles.get(page.document, a).has_z_index);
  CHECK_EQ(page.styles.get(page.document, a).z_index, 0);
}

BONK_TEST(css, float_and_clear_are_parsed) {
  StyledPage page("<body><div id=a></div><div id=b></div><div id=c></div></body>",
                  "#a { float: left } #b { float: right } #c { clear: both }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  const dom::NodeId b = page.document.query_selector(page.document.root(), "#b");
  const dom::NodeId c = page.document.query_selector(page.document.root(), "#c");
  REQUIRE(a.valid() && b.valid() && c.valid());
  CHECK(page.styles.get(page.document, a).float_side == css::FloatSide::Left);
  CHECK(page.styles.get(page.document, a).is_floating());
  CHECK(page.styles.get(page.document, b).float_side == css::FloatSide::Right);
  CHECK(page.styles.get(page.document, c).clear == css::ClearSide::Both);
  CHECK(!page.styles.get(page.document, c).is_floating());
  // `float: none` is the initial value and undoes an earlier declaration.
  CHECK(page.styles.get(page.document, c).float_side == css::FloatSide::None);
}

BONK_TEST(css, float_accepts_the_logical_keywords) {
  StyledPage page("<body><div id=a></div></body>", "#a { float: inline-start }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  REQUIRE(a.valid());
  CHECK(page.styles.get(page.document, a).float_side == css::FloatSide::Left);
}

BONK_TEST(css, overflow_shorthand_and_longhands) {
  StyledPage page("<body><div id=a></div><div id=b></div><div id=c></div></body>",
                  "#a { overflow: hidden } #b { overflow: hidden auto } #c { overflow-x: hidden }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  const dom::NodeId b = page.document.query_selector(page.document.root(), "#b");
  const dom::NodeId c = page.document.query_selector(page.document.root(), "#c");
  REQUIRE(a.valid() && b.valid() && c.valid());
  CHECK(page.styles.get(page.document, a).overflow_x == css::Overflow::Hidden);
  CHECK(page.styles.get(page.document, a).overflow_y == css::Overflow::Hidden);
  CHECK(page.styles.get(page.document, a).clips_overflow());
  CHECK(page.styles.get(page.document, b).overflow_x == css::Overflow::Hidden);
  CHECK(page.styles.get(page.document, b).overflow_y == css::Overflow::Auto);
  // CSS: a `visible` axis next to a clipping one computes to `auto`.
  CHECK(page.styles.get(page.document, c).overflow_x == css::Overflow::Hidden);
  CHECK(page.styles.get(page.document, c).overflow_y == css::Overflow::Auto);
}

BONK_TEST(css, overflow_clip_behaves_like_hidden) {
  StyledPage page("<body><div id=a></div></body>", "#a { overflow: clip }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  REQUIRE(a.valid());
  CHECK(page.styles.get(page.document, a).overflow_x == css::Overflow::Hidden);
  CHECK(page.styles.get(page.document, a).overflow_y == css::Overflow::Hidden);
}

BONK_TEST(css, overflow_unknown_value_keeps_the_previous_one) {
  StyledPage page("<body><div id=a></div></body>", "#a { overflow: hidden } #a { overflow: sideways }");
  const dom::NodeId a = page.document.query_selector(page.document.root(), "#a");
  REQUIRE(a.valid());
  CHECK(page.styles.get(page.document, a).overflow_x == css::Overflow::Hidden);
}
