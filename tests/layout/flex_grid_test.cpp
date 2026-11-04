// Stage 5: flexbox and grid containers.
#include <cmath>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "bonk/css/css.hpp"
#include "bonk/html/html.hpp"
#include "bonk/layout/layout.hpp"

using namespace bonk;

namespace {

struct Fixture {
  dom::Document document;
  css::StyleMap styles;
  text::Font font;
  layout::LayoutResult result;

  Fixture(const std::string& html, const std::string& css, float width = 600.0f, float height = 400.0f) {
    Diagnostics diagnostics;
    CHECK(html::parse_html(html, document, &diagnostics).is_ok());
    Result<css::StyleSheet> sheet = css::StyleSheet::parse("body { margin: 0 } " + css, &diagnostics);
    CHECK(sheet.is_ok());
    styles = css::compute_styles(document, sheet.value(), width, &diagnostics);
    result = layout::layout_document(document, styles, font, layout::LayoutViewport{width, height});
  }

  [[nodiscard]] const layout::Box* box_of(const std::string& selector) {
    const dom::NodeId node = document.query_selector(document.root(), selector);
    if (!node.valid()) return nullptr;
    return find(result.root, node);
  }

  static const layout::Box* find(const layout::Box& box, dom::NodeId node) {
    if (box.node == node && box.kind == layout::BoxKind::Block) return &box;
    for (const layout::Box& child : box.children) {
      if (const layout::Box* found = find(child, node); found != nullptr) return found;
    }
    return nullptr;
  }
};

bool near(float a, float b, float epsilon = 0.6f) { return std::fabs(a - b) <= epsilon; }

}  // namespace

BONK_TEST(flex, grow_splits_the_container_evenly) {
  Fixture fixture(
      "<html><body><div id='row'><div id='a'>a</div><div id='b'>b</div><div id='c'>c</div></div></body></html>",
      "#row { display: flex } #row > div { flex: 1 }");
  const layout::Box* row = fixture.box_of("#row");
  REQUIRE(row != nullptr);
  REQUIRE(row->children.size() == 3);
  const float expected = (row->content_box.width) / 3.0f;
  for (const layout::Box& child : row->children) CHECK(near(child.border_box.width, expected));

  // Left-to-right placement, no gaps.
  CHECK(near(row->children[0].border_box.x, row->content_box.x));
  CHECK(near(row->children[1].border_box.x, row->content_box.x + expected));
  CHECK(near(row->children[2].border_box.x, row->content_box.x + 2.0f * expected));
}

BONK_TEST(flex, gap_justify_content_and_padding) {
  Fixture fixture(
      "<html><body><div id='row'><div>a</div><div>b</div></div></body></html>",
      "#row { display: flex; justify-content: center; gap: 20px; padding: 10px } #row > div { width: 100px }");
  const layout::Box* row = fixture.box_of("#row");
  REQUIRE(row != nullptr);
  REQUIRE(row->children.size() == 2);
  const float total = 100.0f * 2 + 20.0f;
  const float expected_x = row->content_box.x + (row->content_box.width - total) / 2.0f;
  CHECK(near(row->children[0].border_box.x, expected_x));
  CHECK(near(row->children[1].border_box.x, expected_x + 120.0f));
}

BONK_TEST(flex, align_items_center_and_stretch) {
  Fixture fixture(
      "<html><body><div id='row'><div id='short'>s</div><div id='tall'>t<br>t<br>t</div></div></body></html>",
      "#row { display: flex; align-items: center } #row > div { width: 100px }");
  const layout::Box* row = fixture.box_of("#row");
  REQUIRE(row != nullptr);
  REQUIRE(row->children.size() == 2);
  const float line_height = std::max(row->children[0].border_box.height, row->children[1].border_box.height);
  const float short_center = row->children[0].border_box.y + row->children[0].border_box.height / 2.0f;
  const float row_center = row->content_box.y + line_height / 2.0f;
  CHECK(near(short_center, row_center, 1.5f));

  Fixture stretched(
      "<html><body><div id='row'><div id='short'>s</div><div id='tall'>t<br>t<br>t</div></div></body></html>",
      "#row { display: flex } #row > div { width: 100px }");
  const layout::Box* stretch_row = stretched.box_of("#row");
  REQUIRE(stretch_row != nullptr);
  REQUIRE(stretch_row->children.size() == 2);
  CHECK(near(stretch_row->children[0].border_box.height, stretch_row->children[1].border_box.height, 1.5f));
}

BONK_TEST(flex, column_direction_stacks_items) {
  Fixture fixture(
      "<html><body><div id='col'><div>a</div><div>b</div><div>c</div></div></body></html>",
      "#col { display: flex; flex-direction: column; gap: 8px } #col > div { height: 30px }");
  const layout::Box* column = fixture.box_of("#col");
  REQUIRE(column != nullptr);
  REQUIRE(column->children.size() == 3);
  CHECK(near(column->children[0].border_box.y, column->content_box.y));
  CHECK(near(column->children[1].border_box.y, column->content_box.y + 38.0f));
  CHECK(near(column->children[2].border_box.y, column->content_box.y + 76.0f));
  // Cross axis: stretched to the container width.
  CHECK(near(column->children[0].border_box.width, column->content_box.width));
}

BONK_TEST(flex, wrap_breaks_into_lines) {
  Fixture fixture(
      "<html><body><div id='wrap'><div>a</div><div>b</div><div>c</div><div>d</div></div></body></html>",
      "#wrap { display: flex; flex-wrap: wrap; width: 320px } #wrap > div { width: 150px }");
  const layout::Box* wrap = fixture.box_of("#wrap");
  REQUIRE(wrap != nullptr);
  REQUIRE(wrap->children.size() == 4);
  CHECK(near(wrap->children[0].border_box.y, wrap->children[1].border_box.y, 0.6f));
  CHECK(wrap->children[2].border_box.y > wrap->children[0].border_box.y);
  CHECK(near(wrap->children[2].border_box.y, wrap->children[3].border_box.y, 0.6f));
}

BONK_TEST(flex, row_reverse_packs_from_the_end) {
  Fixture fixture(
      "<html><body><div id='row'><div>a</div><div>b</div></div></body></html>",
      "#row { display: flex; flex-direction: row-reverse } #row > div { width: 100px }");
  const layout::Box* row = fixture.box_of("#row");
  REQUIRE(row != nullptr);
  REQUIRE(row->children.size() == 2);
  CHECK(near(row->children[0].border_box.x, row->content_box.x + row->content_box.width - 100.0f));
  CHECK(near(row->children[1].border_box.x, row->content_box.x + row->content_box.width - 200.0f));
}

BONK_TEST(grid, fraction_tracks_split_the_width) {
  Fixture fixture(
      "<html><body><div id='g'><div>a</div><div>b</div><div>c</div></div></body></html>",
      "#g { display: grid; grid-template-columns: 1fr 2fr 1fr }");
  const layout::Box* grid = fixture.box_of("#g");
  REQUIRE(grid != nullptr);
  REQUIRE(grid->children.size() == 3);
  const float width = grid->content_box.width;
  CHECK(near(grid->children[0].border_box.width, width / 4.0f));
  CHECK(near(grid->children[1].border_box.width, width / 2.0f));
  CHECK(near(grid->children[2].border_box.width, width / 4.0f));
}

BONK_TEST(grid, explicit_tracks_gaps_and_rows) {
  Fixture fixture(
      "<html><body><div id='g'><div>a</div><div>b</div><div>c</div><div>d</div></div></body></html>",
      "#g { display: grid; grid-template-columns: 100px 200px; gap: 10px }");
  const layout::Box* grid = fixture.box_of("#g");
  REQUIRE(grid != nullptr);
  REQUIRE(grid->children.size() == 4);
  CHECK(near(grid->children[0].border_box.width, 100.0f));
  CHECK(near(grid->children[1].border_box.width, 200.0f));
  CHECK(near(grid->children[1].border_box.x, grid->children[0].border_box.x + 110.0f));
  // Second row sits below the first one plus the row gap.
  CHECK(near(grid->children[2].border_box.y,
             grid->children[0].border_box.y + grid->children[0].border_box.height + 10.0f, 1.0f));
}

BONK_TEST(grid, repeat_expands_tracks) {
  Fixture fixture(
      "<html><body><div id='g'><div>a</div><div>b</div><div>c</div></div></body></html>",
      "#g { display: grid; grid-template-columns: repeat(3, 1fr) }");
  const layout::Box* grid = fixture.box_of("#g");
  REQUIRE(grid != nullptr);
  REQUIRE(grid->children.size() == 3);
  const float expected = grid->content_box.width / 3.0f;
  for (const layout::Box& child : grid->children) CHECK(near(child.border_box.width, expected));
}

BONK_TEST(flex_grid, parsed_style_exposes_flex_and_grid_properties) {
  Diagnostics diagnostics;
  dom::Document document;
  CHECK(html::parse_html("<html><body><div id='x'>x</div></body></html>", document, &diagnostics).is_ok());
  Result<css::StyleSheet> sheet = css::StyleSheet::parse(
      "#x { display: flex; flex-direction: column; justify-content: space-between; align-items: center;"
      " gap: 6px; flex: 2 3 40px }", &diagnostics);
  REQUIRE(sheet.is_ok());
  const css::StyleMap styles = css::compute_styles(document, sheet.value(), 600.0f, &diagnostics);
  const css::ComputedStyle& style = styles.get(document, document.get_element_by_id("x"));
  CHECK(style.display == css::Display::Flex);
  CHECK(style.flex_direction == css::FlexDirection::Column);
  CHECK(style.justify_content == css::JustifyContent::SpaceBetween);
  CHECK(style.align_items == css::AlignItems::Center);
  CHECK_EQ(style.gap, 6.0f);
  CHECK_EQ(style.flex_grow, 2.0f);
  CHECK_EQ(style.flex_shrink, 3.0f);

  Diagnostics grid_diagnostics;
  dom::Document grid_document;
  CHECK(html::parse_html("<html><body><div id='g'>g</div></body></html>", grid_document, &grid_diagnostics).is_ok());
  Result<css::StyleSheet> grid_sheet = css::StyleSheet::parse(
      "#g { display: grid; grid-template-columns: 120px 1fr auto; row-gap: 4px; column-gap: 8px }",
      &grid_diagnostics);
  REQUIRE(grid_sheet.is_ok());
  const css::StyleMap grid_styles =
      css::compute_styles(grid_document, grid_sheet.value(), 600.0f, &grid_diagnostics);
  const css::ComputedStyle& grid_style = grid_styles.get(grid_document, grid_document.get_element_by_id("g"));
  CHECK(grid_style.display == css::Display::Grid);
  REQUIRE(grid_style.grid_columns.size() == 3);
  CHECK(grid_style.grid_columns[0].unit == css::GridTrack::Unit::Px);
  CHECK_EQ(grid_style.grid_columns[0].value, 120.0f);
  CHECK(grid_style.grid_columns[1].unit == css::GridTrack::Unit::Fr);
  CHECK(grid_style.grid_columns[2].unit == css::GridTrack::Unit::Auto);
  CHECK_EQ(grid_style.row_gap, 4.0f);
  CHECK_EQ(grid_style.column_gap, 8.0f);
}
