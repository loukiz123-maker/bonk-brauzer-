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
    // Out-of-flow boxes hang off their containing block, not off their DOM parent.
    for (const layout::Box& child : box.positioned) {
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

// --- positioning (stage 20) ---------------------------------------------------

BONK_TEST(layout, absolute_insets_place_the_box_inside_a_relative_parent) {
  LayoutFixture fixture(
      "<div id=\"parent\"><div id=\"abs\">x</div></div><div id=\"after\">after</div>",
      "#parent { position: relative; width: 200px; height: 100px; padding: 10px; border: 5px solid #000 }"
      "#abs { position: absolute; left: 20px; top: 30px; width: 50px; height: 40px }");
  const layout::Box* parent = fixture.find_block("#parent");
  const layout::Box* abs = fixture.find_block("#abs");
  REQUIRE(parent != nullptr);
  REQUIRE(abs != nullptr);
  // The containing block is the parent's *padding* box: its content origin is
  // (border 5 + padding 10) = 15, and the padding box starts at the border edge 5.
  const float padding_x = parent->content_box.x - parent->padding[3];
  const float padding_y = parent->content_box.y - parent->padding[0];
  CHECK_EQ(abs->border_box.x, padding_x + 20.0f);
  CHECK_EQ(abs->border_box.y, padding_y + 30.0f);
  CHECK_EQ(abs->border_box.width, 50.0f);
  CHECK_EQ(abs->border_box.height, 40.0f);
  CHECK(abs->out_of_flow);
  CHECK(!abs->fixed);
}

BONK_TEST(layout, absolute_box_leaves_the_flow) {
  LayoutFixture fixture("<div id=\"a\">a</div><div id=\"abs\">x</div><div id=\"b\">b</div>",
                              "div { display: block; height: 20px }"
                              "#abs { position: absolute; top: 300px; left: 0 }");
  // The absolutely positioned div keeps its place in the box tree, but takes no room
  // in the flow: #b stays right below #a.
  const layout::Box* a = fixture.find_block("#a");
  const layout::Box* b = fixture.find_block("#b");
  const layout::Box* abs = fixture.find_block("#abs");
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(abs != nullptr);
  CHECK_EQ(a->border_box.y, 0.0f);
  CHECK_EQ(b->border_box.y, 20.0f);
  CHECK_EQ(abs->border_box.y, 300.0f);
  // ... and the document is tall enough to scroll to it.
  CHECK(fixture.result.document_height >= 320.0f);
}

BONK_TEST(layout, absolute_stretches_between_left_and_right) {
  LayoutFixture fixture("<div id=\"parent\"><div id=\"abs\">x</div></div>",
                        "#parent { position: relative; width: 300px; height: 50px }"
                        "#abs { position: absolute; left: 20px; right: 40px; top: 0 }");
  const layout::Box* abs = fixture.find_block("#abs");
  REQUIRE(abs != nullptr);
  CHECK_EQ(abs->border_box.x, 20.0f);
  CHECK_EQ(abs->border_box.width, 240.0f);  // 300 - 20 - 40
}

BONK_TEST(layout, absolute_shrinks_to_fit_and_stays_inside) {
  LayoutFixture tight("<div id=\"parent\"><div id=\"abs\">x</div></div>",
                            "#parent { position: relative; width: 100px; height: 50px }"
                            "#abs { position: absolute; left: 60px }");
  const layout::Box* abs = tight.find_block("#abs");
  REQUIRE(abs != nullptr);
  CHECK(abs->border_box.width <= 40.0f);  // shrink-to-fit, clamped by what is left
  CHECK(abs->border_box.width > 0.0f);

  LayoutFixture loose("<div id=\"parent\"><div id=\"abs\">hello world</div></div>",
                            "#parent { position: relative; width: 500px; height: 50px }"
                            "#abs { position: absolute; left: 0 }");
  const layout::Box* fitted = loose.find_block("#abs");
  REQUIRE(fitted != nullptr);
  CHECK(fitted->border_box.width > 0.0f);
  CHECK(fitted->border_box.width < 500.0f);  // as wide as the content, not the parent
}

BONK_TEST(layout, fixed_is_anchored_to_the_viewport) {
  LayoutFixture fixture(
      "<div style=\"margin-left:100px;margin-top:50px\"><div style=\"margin-left:100px\">"
      "<div id=\"fixed\">bar</div></div></div>",
      "#fixed { position: fixed; left: 0; top: 0; width: 80px; height: 20px }");
  const layout::Box* fixed = fixture.find_block("#fixed");
  REQUIRE(fixed != nullptr);
  CHECK_EQ(fixed->border_box.x, 0.0f);
  CHECK_EQ(fixed->border_box.y, 0.0f);
  CHECK(fixed->fixed);
  // A fixed box does not make the document taller.
  CHECK(fixture.result.document_height < 100.0f);
}

BONK_TEST(layout, fixed_bottom_right_uses_the_viewport_size) {
  LayoutFixture fixture("<div id=\"fixed\">x</div>",
                              "#fixed { position: fixed; right: 10px; bottom: 20px; width: 100px; height: 30px }");
  const layout::Box* fixed = fixture.find_block("#fixed");
  REQUIRE(fixed != nullptr);
  CHECK_EQ(fixed->border_box.x, 600.0f - 10.0f - 100.0f);
  CHECK_EQ(fixed->border_box.y, 400.0f - 20.0f - 30.0f);
}

BONK_TEST(layout, relative_offsets_move_the_box_but_not_its_siblings) {
  LayoutFixture fixture("<div id=\"a\">a</div><div id=\"rel\">r</div><div id=\"b\">b</div>",
                              "div { display: block; height: 20px }"
                              "#rel { position: relative; left: 15px; top: 5px }");
  const layout::Box* a = fixture.find_block("#a");
  const layout::Box* rel = fixture.find_block("#rel");
  const layout::Box* b = fixture.find_block("#b");
  REQUIRE(a != nullptr);
  REQUIRE(rel != nullptr);
  REQUIRE(b != nullptr);
  CHECK_EQ(a->border_box.y, 0.0f);
  CHECK_EQ(rel->border_box.y, 25.0f);  // 20 + 5
  CHECK_EQ(rel->border_box.x, 15.0f);
  CHECK_EQ(b->border_box.y, 40.0f);  // the sibling does not move
}

BONK_TEST(layout, relative_negative_offset_uses_the_opposite_inset) {
  LayoutFixture fixture("<div id=\"rel\">r</div>",
                              "#rel { display: block; position: relative; right: 10px; bottom: 4px }");
  const layout::Box* rel = fixture.find_block("#rel");
  REQUIRE(rel != nullptr);
  CHECK_EQ(rel->border_box.x, -10.0f);
  CHECK_EQ(rel->border_box.y, -4.0f);
}

BONK_TEST(layout, containing_block_is_the_nearest_positioned_ancestor) {
  LayoutFixture fixture(
      "<div id=\"outer\"><div id=\"inner\"><div id=\"abs\">x</div></div></div>",
      "#outer { position: relative; margin-left: 40px; width: 300px; height: 100px; padding: 10px }"
      "#inner { margin-left: 25px; height: 50px }"
      "#abs { position: absolute; left: 5px; top: 6px; width: 10px; height: 10px }");
  const layout::Box* outer = fixture.find_block("#outer");
  const layout::Box* abs = fixture.find_block("#abs");
  REQUIRE(outer != nullptr);
  REQUIRE(abs != nullptr);
  // #inner is static, so the offset is measured from #outer's padding box (its
  // border edge: no border here) and the intermediate margin does not count.
  const float padding_x = outer->content_box.x - outer->padding[3];
  const float padding_y = outer->content_box.y - outer->padding[0];
  CHECK_EQ(abs->border_box.x, padding_x + 5.0f);
  CHECK_EQ(abs->border_box.y, padding_y + 6.0f);
}

BONK_TEST(layout, z_index_is_recorded_for_positioned_boxes_only) {
  LayoutFixture fixture("<div id=\"parent\"><div id=\"low\"></div><div id=\"high\"></div></div>",
                              "#parent { position: relative; width: 100px; height: 100px }"
                              "#low { position: absolute; z-index: 2; width: 10px; height: 10px }"
                              "#high { position: absolute; z-index: 9; width: 10px; height: 10px }"
                              "div#parent div { display: block }");
  const layout::Box* low = fixture.find_block("#low");
  const layout::Box* high = fixture.find_block("#high");
  REQUIRE(low != nullptr);
  REQUIRE(high != nullptr);
  CHECK_EQ(low->z_index, 2);
  CHECK_EQ(high->z_index, 9);
}

BONK_TEST(layout, hit_test_prefers_positioned_boxes) {
  LayoutFixture fixture("<div id=\"under\">u</div><div id=\"over\">o</div>",
                              "div { display: block; height: 50px }"
                              "#over { position: absolute; top: 0; left: 0; width: 50px; height: 50px }");
  const dom::NodeId over = fixture.document.query_selector(fixture.document.root(), "#over");
  const dom::NodeId under = fixture.document.query_selector(fixture.document.root(), "#under");
  REQUIRE(over.valid());
  REQUIRE(under.valid());
  // Both boxes cover (10, 10); the positioned one paints on top, so it wins the click.
  const layout::Box* hit = layout::hit_test(fixture.result.root, 10.0f, 10.0f);
  REQUIRE(hit != nullptr);
  CHECK(hit->node == over);
  // Somewhere the positioned box does not cover, the flow box is found.
  const layout::Box* below = layout::hit_test(fixture.result.root, 400.0f, 10.0f);
  REQUIRE(below != nullptr);
  CHECK(below->node == under);
}

BONK_TEST(layout, absolute_inside_inline_block_parent) {
  LayoutFixture fixture("<div id=\"ib\"><span id=\"abs\">x</span></div>",
                              "#ib { display: inline-block; position: relative; width: 100px; height: 40px }"
                              "#abs { position: absolute; left: 10px; top: 10px; width: 20px; height: 20px }");
  const layout::Box* abs = fixture.find_block("#abs");
  REQUIRE(abs != nullptr);
  CHECK_EQ(abs->border_box.x, 10.0f);
  CHECK_EQ(abs->border_box.y, 10.0f);
}

// --- floats (stage 21) --------------------------------------------------------

BONK_TEST(layout, float_leaves_the_flow_and_text_wraps_around_it) {
  LayoutFixture fixture(
      "<div id=\"f\"></div><div id=\"p\">aaaaa bbbbb ccccc ddddd eeeee fffff ggggg hhhhh</div>",
      "div { display: block }"
      "#f { float: left; width: 80px; height: 40px; background: #336 }"
      "#p { width: 200px }");
  const layout::Box* f = fixture.find_block("#f");
  const layout::Box* p = fixture.find_block("#p");
  REQUIRE(f != nullptr);
  REQUIRE(p != nullptr);
  CHECK_EQ(f->border_box.x, 0.0f);
  CHECK_EQ(f->border_box.y, 0.0f);
  // The paragraph's own box still spans the full width (block boxes overlap floats) ...
  CHECK_EQ(p->border_box.x, 0.0f);
  CHECK_EQ(p->border_box.y, 0.0f);
  CHECK_EQ(p->border_box.width, 200.0f);
  // ... but its first line starts to the right of the float.
  const std::vector<const layout::Box*> texts = LayoutFixture::text_boxes(*p);
  REQUIRE(texts.size() > 1);
  CHECK_EQ(texts.front()->border_box.x, 80.0f);
  CHECK_EQ(texts.front()->border_box.y, 0.0f);
  // A line below the float gets the full width back.
  bool wrapped_back = false;
  for (const layout::Box* text : texts) {
    if (text->border_box.y >= 40.0f) {
      wrapped_back = text->border_box.x < 80.0f;
      break;
    }
  }
  CHECK(wrapped_back);
}

BONK_TEST(layout, float_right_sits_on_the_right_edge) {
  LayoutFixture fixture("<div id=\"f\">x</div>", "#f { float: right; width: 90px; height: 20px }");
  const layout::Box* f = fixture.find_block("#f");
  REQUIRE(f != nullptr);
  CHECK_EQ(f->border_box.x, 600.0f - 90.0f);
  CHECK_EQ(f->border_box.y, 0.0f);
}

BONK_TEST(layout, floats_share_a_line_until_they_no_longer_fit) {
  LayoutFixture fixture(
      "<div id=\"wrap\"><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div></div>",
      "#wrap { width: 220px }"
      "div div { float: left; width: 100px; height: 20px }");
  const layout::Box* a = fixture.find_block("#a");
  const layout::Box* b = fixture.find_block("#b");
  const layout::Box* c = fixture.find_block("#c");
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(c != nullptr);
  CHECK_EQ(a->border_box.x, 0.0f);
  CHECK_EQ(b->border_box.x, 100.0f);
  // Only 20px are left on that line: the third float drops below the first.
  CHECK_EQ(c->border_box.x, 0.0f);
  CHECK_EQ(c->border_box.y, 20.0f);
}

BONK_TEST(layout, clear_pushes_a_block_below_the_floats) {
  LayoutFixture fixture(
      "<div id=\"f\"></div><div id=\"mid\">mid</div><div id=\"after\">after</div>",
      "div { display: block }"
      "#f { float: left; width: 50px; height: 45px }"
      "#mid { height: 10px }"
      "#after { clear: left; height: 12px }");
  const layout::Box* f = fixture.find_block("#f");
  const layout::Box* mid = fixture.find_block("#mid");
  const layout::Box* after = fixture.find_block("#after");
  REQUIRE(f != nullptr);
  REQUIRE(mid != nullptr);
  REQUIRE(after != nullptr);
  CHECK_EQ(mid->border_box.y, 0.0f);  // a block does not move out of a float's way
  CHECK_EQ(after->border_box.y, 45.0f);  // `clear: left` does
}

BONK_TEST(layout, float_shrinks_to_fit_its_content) {
  LayoutFixture fixture("<div id=\"f\">a short sentence</div>",
                        "#f { float: left; padding: 4px }");
  const layout::Box* f = fixture.find_block("#f");
  REQUIRE(f != nullptr);
  CHECK(f->border_box.width > 8.0f);   // content + padding
  CHECK(f->border_box.width < 200.0f);  // far narrower than the page
  CHECK_EQ(f->border_box.width, f->content_box.width + 8.0f);
}

BONK_TEST(layout, floats_extend_the_document_but_not_their_parent) {
  LayoutFixture fixture("<div id=\"wrap\"><div id=\"f\"></div></div>",
                        "#f { float: left; width: 60px; height: 300px }"
                        "#wrap { width: 200px }");
  const layout::Box* wrap = fixture.find_block("#wrap");
  REQUIRE(wrap != nullptr);
  // `overflow: visible` does not contain floats: this is the classic reason the
  // clearfix exists, and browsers behave the same way.
  CHECK_EQ(wrap->border_box.height, 0.0f);
  // The page itself still has to be scrollable to the bottom of the float.
  CHECK(fixture.result.document_height >= 300.0f);
}

BONK_TEST(layout, absolutely_positioned_box_ignores_outer_floats) {
  LayoutFixture fixture("<div id=\"f\"></div><div id=\"abs\">abs</div>",
                        "div { display: block }"
                        "#f { float: left; width: 120px; height: 120px }"
                        "#abs { position: absolute; left: 0; top: 0; width: 30px; height: 30px }");
  const layout::Box* abs = fixture.find_block("#abs");
  REQUIRE(abs != nullptr);
  CHECK_EQ(abs->border_box.x, 0.0f);  // a float does not shift positioned boxes
  CHECK_EQ(abs->border_box.width, 30.0f);
}

BONK_TEST(layout, floated_image_inside_a_paragraph_wraps_the_text) {
  LayoutFixture fixture(
      "<p id=\"p\"><img src=\"x.png\" width=\"100\" height=\"50\" style=\"float:left\">"
      "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp"
      " qqqq rrrr ssss tttt</p>",
      "p { margin: 0; width: 300px }");
  const layout::Box* p = fixture.find_block("#p");
  REQUIRE(p != nullptr);
  // The floated image sits at the top-left corner of the paragraph and is sized from
  // its width/height attributes, not from measuring its (empty) content.
  const dom::NodeId image = fixture.document.query_selector(fixture.document.root(), "img");
  REQUIRE(image.valid());
  const layout::Box* float_box = LayoutFixture::find_by_node(*p, image);
  REQUIRE(float_box != nullptr);
  CHECK_EQ(float_box->border_box.width, 100.0f);
  CHECK_EQ(float_box->border_box.height, 50.0f);
  // Text starts to the right of it, and returns to the left edge below it.
  const std::vector<const layout::Box*> texts = LayoutFixture::text_boxes(*p);
  REQUIRE(!texts.empty());
  CHECK_EQ(texts.front()->border_box.x, 100.0f);
  bool wrapped_back = false;
  for (const layout::Box* text : texts) {
    if (text->border_box.y >= 50.0f && text->border_box.x < 100.0f) {
      wrapped_back = true;
      break;
    }
  }
  CHECK(wrapped_back);
}

// --- overflow (stage 22) ------------------------------------------------------

BONK_TEST(layout, overflow_hidden_clips_to_the_padding_box) {
  LayoutFixture fixture("<div id=\"clip\"><div id=\"inner\"></div></div>",
                        "div { display: block }"
                        "#clip { width: 100px; height: 50px; padding: 5px; border: 2px solid #000; overflow: hidden }"
                        "#inner { width: 300px; height: 300px }");
  const layout::Box* clip = fixture.find_block("#clip");
  REQUIRE(clip != nullptr);
  REQUIRE(clip->has_clip);
  // Padding box: the border is outside the clip, the padding inside.
  CHECK_EQ(clip->clip.x, clip->border_box.x + clip->border[3]);
  CHECK_EQ(clip->clip.y, clip->border_box.y + clip->border[0]);
  CHECK_EQ(clip->clip.width, 100.0f + 10.0f);
  CHECK_EQ(clip->clip.height, 50.0f + 10.0f);
}

BONK_TEST(layout, overflow_visible_does_not_clip) {
  LayoutFixture fixture("<div id=\"no\">x</div>", "#no { width: 40px; height: 20px }");
  const layout::Box* box = fixture.find_block("#no");
  REQUIRE(box != nullptr);
  CHECK(!box->has_clip);
}

BONK_TEST(layout, overflow_hidden_contains_floats) {
  LayoutFixture fixture("<div id=\"bfc\"><div id=\"f\"></div></div><div id=\"after\">after</div>",
                        "div { display: block }"
                        "#bfc { overflow: hidden; width: 200px }"
                        "#f { float: left; width: 50px; height: 45px }"
                        "#after { height: 10px }");
  const layout::Box* bfc = fixture.find_block("#bfc");
  const layout::Box* after = fixture.find_block("#after");
  REQUIRE(bfc != nullptr);
  REQUIRE(after != nullptr);
  // The BFC root grows to contain its float — the second way to "clear", next to
  // `clear: both`. With `overflow: visible` this height would be 0 (see stage 21).
  CHECK_EQ(bfc->border_box.height, 45.0f);
  CHECK_EQ(after->border_box.y, 45.0f);
}

BONK_TEST(layout, a_single_axis_overflow_still_clips_both) {
  LayoutFixture fixture("<div id=\"clip\"><div id=\"inner\"></div></div>",
                        "div { display: block }"
                        "#clip { width: 80px; height: 40px; overflow-x: hidden }"
                        "#inner { width: 200px; height: 200px }");
  const layout::Box* clip = fixture.find_block("#clip");
  REQUIRE(clip != nullptr);
  REQUIRE(clip->has_clip);
  // `overflow-x: hidden` computes the other axis to `auto` (see the CSS test), and an
  // `auto` axis clips as well: the clip is the padding box on both axes.
  CHECK_EQ(clip->clip.width, 80.0f);
  CHECK_EQ(clip->clip.height, 40.0f);
}

BONK_TEST(layout, hit_test_ignores_clipped_content) {
  LayoutFixture fixture("<div id=\"clip\"><div id=\"inner\"></div></div>",
                        "div { display: block }"
                        "#clip { width: 60px; height: 30px; overflow: hidden }"
                        "#inner { width: 200px; height: 200px }");
  const dom::NodeId inner = fixture.document.query_selector(fixture.document.root(), "#inner");
  REQUIRE(inner.valid());
  // Inside the box the inner element wins (it is deeper) ...
  const layout::Box* inside = layout::hit_test(fixture.result.root, 20.0f, 10.0f);
  REQUIRE(inside != nullptr);
  CHECK(inside->node == inner);
  // ... but not outside the clip rectangle.
  const layout::Box* outside = layout::hit_test(fixture.result.root, 120.0f, 100.0f);
  CHECK(outside == nullptr);
}
