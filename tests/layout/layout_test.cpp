#include "test_framework.hpp"

#include "bonk/css/css.hpp"
#include "bonk/html/html.hpp"
#include "bonk/layout/layout.hpp"

using namespace bonk;

namespace {

struct LayoutFixture {
  dom::Document document;
  css::StyleMap styles;
  text::Font font;
  layout::LayoutResult result;

  LayoutFixture(std::string_view html, std::string_view css, float width = 600.0f, float height = 400.0f) {
    Diagnostics diagnostics;
    CHECK(html::parse_html(html, document, &diagnostics).is_ok());
    // Every fixture starts from a page with no body margin, so the y positions
    // below are absolute and easy to reason about.
    Result<css::StyleSheet> sheet = css::StyleSheet::parse("body { margin: 0 } " + std::string(css), &diagnostics);
    CHECK(sheet.is_ok());
    styles = css::compute_styles(document, sheet.value(), width, &diagnostics);
    result = layout::layout_document(document, styles, font, layout::LayoutViewport{width, height});
  }

  [[nodiscard]] const layout::Box* find_block(std::string_view selector) {
    const dom::NodeId node = document.query_selector(document.root(), selector);
    if (!node.valid()) return nullptr;
    return find_by_node(result.root, node);
  }

  static const layout::Box* find_by_node(const layout::Box& box, dom::NodeId node) {
    if (box.node == node && box.kind == layout::BoxKind::Block) return &box;
    for (const layout::Box& child : box.children) {
      if (const layout::Box* found = find_by_node(child, node); found != nullptr) return found;
    }
    return nullptr;
  }

  [[nodiscard]] static std::vector<const layout::Box*> text_boxes(const layout::Box& box) {
    std::vector<const layout::Box*> out;
    for (const layout::Box& child : box.children) {
      if (child.kind == layout::BoxKind::Text) {
        out.push_back(&child);
      } else {
        for (const layout::Box* nested : text_boxes(child)) out.push_back(nested);
      }
    }
    return out;
  }
};

}  // namespace

BONK_TEST(layout, blocks_stack_vertically) {
  LayoutFixture fixture("<div id=\"a\">a</div><div id=\"b\">b</div>", "div { display: block; height: 50px }");
  const layout::Box* a = fixture.find_block("#a");
  const layout::Box* b = fixture.find_block("#b");
  REQUIRE(a != nullptr && b != nullptr);
  CHECK_EQ(static_cast<int>(a->border_box.y), 0);
  CHECK_EQ(static_cast<int>(a->border_box.height), 50);
  CHECK_EQ(static_cast<int>(b->border_box.y), 50);
}

BONK_TEST(layout, width_fills_containing_block_and_padding_is_inside) {
  LayoutFixture fixture("<div id=\"a\">x</div>", "div { height: 10px; padding: 5px; border: 2px solid #000 }");
  const layout::Box* a = fixture.find_block("#a");
  REQUIRE(a != nullptr);
  CHECK_EQ(static_cast<int>(a->border_box.width), 600);
  CHECK_EQ(static_cast<int>(a->content_box.width), 600 - 10 - 4);
  CHECK_EQ(static_cast<int>(a->border_box.height), 10 + 10 + 4);
}

BONK_TEST(layout, explicit_width_is_honoured) {
  LayoutFixture fixture("<div id=\"a\">x</div>", "div { width: 200px; height: 10px }");
  const layout::Box* a = fixture.find_block("#a");
  REQUIRE(a != nullptr);
  CHECK_EQ(static_cast<int>(a->border_box.width), 200);
}

BONK_TEST(layout, vertical_margins_collapse) {
  LayoutFixture fixture("<div id=\"a\">a</div><div id=\"b\">b</div>",
                        "div { height: 20px } #a { margin-bottom: 30px } #b { margin-top: 10px }");
  const layout::Box* a = fixture.find_block("#a");
  const layout::Box* b = fixture.find_block("#b");
  REQUIRE(a != nullptr && b != nullptr);
  CHECK_EQ(static_cast<int>(a->border_box.y), 0);
  // max(30, 10) = 30, not 40
  CHECK_EQ(static_cast<int>(b->border_box.y), 20 + 30);
}

BONK_TEST(layout, inline_text_wraps_into_lines) {
  std::string sentence;
  for (int i = 0; i < 12; ++i) sentence += "word ";
  LayoutFixture fixture("<p>" + sentence + "</p>", "p { font-size: 16px }", 200.0f);
  const std::vector<const layout::Box*> lines = LayoutFixture::text_boxes(fixture.result.root);
  CHECK(lines.size() >= 3);  // wrapped, not one endless line
  float previous_y = -1.0f;
  for (const layout::Box* line : lines) {
    CHECK(line->border_box.x >= 0.0f);
    CHECK(line->border_box.right() <= 200.5f);
    CHECK(line->border_box.y >= previous_y);
    previous_y = line->border_box.y;
  }
}

BONK_TEST(layout, text_align_center_and_right) {
  LayoutFixture centered("<p id=\"c\">hi</p>", "p { text-align: center; font-size: 20px }", 400.0f);
  const layout::Box* paragraph = centered.find_block("#c");
  REQUIRE(paragraph != nullptr);
  const std::vector<const layout::Box*> center_lines = LayoutFixture::text_boxes(*paragraph);
  REQUIRE(!center_lines.empty());
  CHECK(center_lines[0]->border_box.x > 50.0f);

  LayoutFixture right("<p id=\"r\">hi</p>", "p { text-align: right; font-size: 20px }", 400.0f);
  const layout::Box* right_paragraph = right.find_block("#r");
  REQUIRE(right_paragraph != nullptr);
  const std::vector<const layout::Box*> right_lines = LayoutFixture::text_boxes(*right_paragraph);
  REQUIRE(!right_lines.empty());
  CHECK(right_lines[0]->border_box.right() > 380.0f);
}

BONK_TEST(layout, display_none_takes_no_space) {
  LayoutFixture fixture("<div id=\"a\">hidden</div><div id=\"b\">shown</div>",
                        "div { height: 30px } #a { display: none }");
  CHECK(fixture.find_block("#a") == nullptr);
  const layout::Box* b = fixture.find_block("#b");
  REQUIRE(b != nullptr);
  CHECK_EQ(static_cast<int>(b->border_box.y), 0);
}

BONK_TEST(layout, list_items_get_markers) {
  LayoutFixture fixture("<ul><li>one</li></ul>", "");
  const layout::Box* item = fixture.find_block("li");
  REQUIRE(item != nullptr);
  CHECK(item->has_marker);
  CHECK(item->marker_x < item->content_box.x);
}

BONK_TEST(layout, image_becomes_atomic_inline_with_attribute_size) {
  LayoutFixture fixture("<p>before <img src=\"x.png\" width=\"40\" height=\"20\"> after</p>", "");
  const layout::Box* paragraph = fixture.find_block("p");
  REQUIRE(paragraph != nullptr);
  bool found_image = false;
  for (const layout::Box& child : paragraph->children) {
    if (child.kind == layout::BoxKind::Image) {
      found_image = true;
      CHECK_EQ(static_cast<int>(child.border_box.width), 40);
      CHECK_EQ(static_cast<int>(child.border_box.height), 20);
    }
  }
  CHECK(found_image);
}

BONK_TEST(layout, hit_test_finds_deepest_box) {
  LayoutFixture fixture("<div id=\"outer\"><p id=\"inner\">text</p></div>",
                        "div { height: 100px } p { height: 40px; margin: 0 }");
  const layout::Box* inner = fixture.find_block("#inner");
  REQUIRE(inner != nullptr);
  const layout::Box* hit = layout::hit_test(fixture.result.root, 10.0f, 10.0f);
  REQUIRE(hit != nullptr);
  // The deepest box at that point is the text run inside <p>, which must live
  // within the paragraph's border box.
  CHECK(hit->border_box.x >= inner->border_box.x);
  CHECK(hit->border_box.y >= inner->border_box.y);
  CHECK(hit->border_box.bottom() <= inner->border_box.bottom() + 0.5f);
  CHECK(layout::hit_test(fixture.result.root, 5000.0f, 5000.0f) == nullptr);
}

BONK_TEST(layout, document_height_grows_with_content) {
  LayoutFixture small("<p>a</p>", "");
  LayoutFixture large("<div style=\"height: 500px\">x</div><div style=\"height: 500px\">y</div>", "");
  CHECK(large.result.document_height > small.result.document_height);
  CHECK(large.result.document_height >= 1000.0f);
}

BONK_TEST(layout, inline_elements_inherit_style_and_stay_on_line) {
  LayoutFixture fixture("<p><b>bold</b> <i>italic</i> plain</p>", "p { font-size: 16px }", 800.0f);
  const std::vector<const layout::Box*> lines = LayoutFixture::text_boxes(fixture.result.root);
  CHECK(lines.size() >= 3);  // one run per inline element plus the trailing text
  float last_x = -1.0f;
  for (const layout::Box* line : lines) last_x = std::max(last_x, line->border_box.right());
  CHECK(last_x > 30.0f);
}
