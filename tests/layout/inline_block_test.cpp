// Inline-block controls, anonymous blocks and hard line breaks (stage 5).
//
// These are the layout features that make `<input>`, `<button>` and mixed
// `text <b>x</b> <input>` content look like a browser instead of a document dump.
#include "bonk/css/css.hpp"
#include "bonk/html/html.hpp"
#include "bonk/layout/layout.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

struct Fixture {
  dom::Document document;
  css::StyleMap styles;
  text::Font font;
  layout::LayoutResult result;

  Fixture(std::string_view html, std::string_view css, float width = 600.0f, float height = 400.0f) {
    Diagnostics diagnostics;
    CHECK(html::parse_html(html, document, &diagnostics).is_ok());
    Result<css::StyleSheet> sheet = css::StyleSheet::parse("body { margin: 0 } " + std::string(css), &diagnostics);
    CHECK(sheet.is_ok());
    styles = css::compute_styles(document, sheet.value(), width, &diagnostics);
    result = layout::layout_document(document, styles, font, layout::LayoutViewport{width, height});
  }

  [[nodiscard]] const layout::Box* box_of(std::string_view selector) {
    const dom::NodeId node = document.query_selector(document.root(), selector);
    if (!node.valid()) return nullptr;
    return find(result.root, node);
  }

  static const layout::Box* find(const layout::Box& box, dom::NodeId node) {
    if (box.node == node && box.kind != layout::BoxKind::Text) return &box;
    for (const layout::Box& child : box.children) {
      if (const layout::Box* found = find(child, node); found != nullptr) return found;
    }
    return nullptr;
  }

  [[nodiscard]] float text_y(std::string_view needle) {
    for (const layout::Box& child : result.root.children) {
      for (const layout::Box& line : child.children) {
        if (line.kind == layout::BoxKind::Text && line.text == needle) return line.border_box.y;
        for (const layout::Box& nested : line.children) {
          if (nested.kind == layout::BoxKind::Text && nested.text == needle) return nested.border_box.y;
        }
      }
      if (child.kind == layout::BoxKind::Text && child.text == needle) return child.border_box.y;
    }
    return -1.0f;
  }
};

}  // namespace

BONK_TEST(inline_block, two_controls_share_a_line_and_do_not_overlap) {
  Fixture fixture(
      "<div><input id=\"a\" value=\"one\"><input id=\"b\" value=\"two\"></div>",
      "input { display: inline-block; min-width: 120px; min-height: 20px; padding: 2px; border: 1px solid #000 }");
  const layout::Box* a = fixture.box_of("#a");
  const layout::Box* b = fixture.box_of("#b");
  REQUIRE(a != nullptr && b != nullptr);
  CHECK_EQ(static_cast<int>(a->border_box.width), 126);  // 120 + 2*2 padding + 2*1 border
  CHECK_EQ(static_cast<int>(a->border_box.height), 26);
  CHECK_EQ(static_cast<int>(a->border_box.y), static_cast<int>(b->border_box.y));
  CHECK(b->border_box.x >= a->border_box.right());
  // The content box sits inside the padding and the border.
  CHECK_EQ(static_cast<int>(a->content_box.x - a->border_box.x), 3);
}

BONK_TEST(inline_block, min_width_and_min_height_are_honoured) {
  Fixture fixture("<div><span id=\"pill\">x</span></div>",
                  "span { display: inline-block; min-width: 80px; min-height: 24px }");
  const layout::Box* pill = fixture.box_of("#pill");
  REQUIRE(pill != nullptr);
  CHECK_EQ(static_cast<int>(pill->border_box.width), 80);
  CHECK(pill->border_box.height >= 24.0f);
}

BONK_TEST(inline_block, declaration_width_wins_over_content) {
  Fixture fixture("<div><input id=\"wide\" value=\"value\" style=\"width: 240px\"></div>",
                  "input { display: inline-block; padding: 2px; border: 1px solid #000 }");
  const layout::Box* wide = fixture.box_of("#wide");
  REQUIRE(wide != nullptr);
  CHECK_EQ(static_cast<int>(wide->border_box.width), 246);
}

BONK_TEST(inline_block, br_starts_a_new_line) {
  Fixture fixture("<div>alpha<br>beta</div>", "div { display: block }");
  const float first = fixture.text_y("alpha");
  const float second = fixture.text_y("beta");
  CHECK(first >= 0.0f);
  CHECK(second > first);
}

BONK_TEST(anonymous_blocks, inline_and_block_content_stay_in_document_order) {
  // `before` is inline, then a block, then `after` inline again: without
  // anonymous blocks both text runs would be glued together below the block.
  Fixture fixture("<div>before<div id=\"middle\">middle</div>after</div>",
                  "div { display: block } #middle { height: 40px }");
  const layout::Box* middle = fixture.box_of("#middle");
  REQUIRE(middle != nullptr);
  const float before = fixture.text_y("before");
  const float after = fixture.text_y("after");
  CHECK(before >= 0.0f);
  CHECK(before < middle->border_box.y);
  CHECK(after >= middle->border_box.bottom());
}

BONK_TEST(anonymous_blocks, label_then_control_then_label_order) {
  Fixture fixture(
      "<div><label id=\"l1\">First</label><input id=\"i1\" value=\"1\">"
      "<label id=\"l2\">Second</label><input id=\"i2\" value=\"2\"></div>",
      "label { display: block } input { display: inline-block; min-width: 100px; min-height: 18px }");
  const layout::Box* l1 = fixture.box_of("#l1");
  const layout::Box* i1 = fixture.box_of("#i1");
  const layout::Box* l2 = fixture.box_of("#l2");
  const layout::Box* i2 = fixture.box_of("#i2");
  REQUIRE(l1 != nullptr && i1 != nullptr && l2 != nullptr && i2 != nullptr);
  CHECK(i1->border_box.y >= l1->border_box.bottom() - 0.5f);
  CHECK(l2->border_box.y >= i1->border_box.bottom() - 0.5f);
  CHECK(i2->border_box.y >= l2->border_box.bottom() - 0.5f);
}
