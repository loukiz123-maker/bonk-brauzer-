// Bonk Brauzer — CSS: parsing, cascade, computed values.
//
// Supported subset (stage 2 scope, everything below is implemented):
//   * selectors: `dom::parse_selector_list` (type/class/id/attr/combinators/pseudo)
//   * declaration blocks, `!important`, longhand + margin/padding/border/background shorthands
//   * lengths px/pt/pc/in/cm/mm/em/rem/%/auto, colors #rgb/#rrggbb/#rrggbbaa/rgb()/rgba()/named
//   * inheritance of font/color/text properties, specificity-ordered cascade
//   * `@media` with `screen`/`print`/`all` and `(min-width)`/`(max-width)`
//   * `@font-face` is parsed and surfaced, but the font is loaded by the text module
// Not implemented (documented, not stubbed): CSS custom properties `var()`, `@supports`,
// animations/transitions, `calc()`, pseudo-elements, sibling combinators.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/diag.hpp"
#include "bonk/core/result.hpp"
#include "bonk/dom/dom.hpp"

namespace bonk::css {

struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  [[nodiscard]] bool is_transparent() const noexcept { return a == 0; }
  friend bool operator==(const Color& a, const Color& b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
  }
  [[nodiscard]] std::string to_string() const;
};

struct Length {
  enum class Unit { Auto, Px, Percent, Em, Rem, Pt, Cm, Mm, In, Pc };
  float value = 0.0f;
  Unit unit = Unit::Px;

  [[nodiscard]] bool is_auto() const noexcept { return unit == Unit::Auto; }
  [[nodiscard]] bool is_absolute() const noexcept {
    return unit == Unit::Px || unit == Unit::Pt || unit == Unit::Cm || unit == Unit::Mm || unit == Unit::In ||
           unit == Unit::Pc;
  }
  [[nodiscard]] static Length px(float value) noexcept { return Length{value, Unit::Px}; }
  [[nodiscard]] static Length percent(float value) noexcept { return Length{value, Unit::Percent}; }
  [[nodiscard]] static Length auto_value() noexcept { return Length{0.0f, Unit::Auto}; }
  [[nodiscard]] std::string to_string() const;
};

enum class Display { Inline, Block, InlineBlock, Flex, Grid, ListItem, None, Table, TableRow, TableCell };

// --- flexbox / grid (stage 5) -------------------------------------------------
enum class FlexDirection { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap { NoWrap, Wrap, WrapReverse };
enum class JustifyContent { FlexStart, Center, FlexEnd, SpaceBetween, SpaceAround, SpaceEvenly };
enum class AlignItems { Stretch, FlexStart, Center, FlexEnd };
enum class AlignContent { FlexStart, Center, FlexEnd, Stretch, SpaceBetween, SpaceAround, SpaceEvenly };

/// One explicit grid track: a length, a fraction of the free space, or auto.
struct GridTrack {
  enum class Unit { Px, Percent, Fr, Auto };
  Unit unit = Unit::Auto;
  float value = 0.0f;
};
enum class TextAlign { Left, Right, Center, Justify };
enum class TextDecoration { None, Underline };

/// Fully resolved style for one element ("computed values" in CSS terms, with
/// percentages still symbolic — percentages need a containing block, so layout
/// resolves them).
struct ComputedStyle {
  Display display = Display::Inline;
  Color color{0, 0, 0, 255};
  Color background_color{0, 0, 0, 0};
  float font_size = 16.0f;  // px
  std::string font_family = "sans-serif";
  int font_weight = 400;
  bool italic = false;
  float line_height = 0.0f;  // px; 0 means "normal" (1.2 * font_size)
  TextAlign text_align = TextAlign::Left;
  TextDecoration text_decoration = TextDecoration::None;
  bool white_space_pre = false;
  bool visible = true;

  Length margin[4]{};   // top, right, bottom, left
  Length padding[4]{};  // top, right, bottom, left
  float border_width[4] = {0, 0, 0, 0};
  Color border_color{0, 0, 0, 0};
  Length width = Length::auto_value();   // CSS initial value
  Length height = Length::auto_value();  // CSS initial value
  float min_height = 0.0f;
  float min_width = 0.0f;   // content box
  float max_width = -1.0f;  // -1 = none
  bool list_item = false;
  float opacity = 1.0f;

  // Flexbox container / item properties.
  FlexDirection flex_direction = FlexDirection::Row;
  FlexWrap flex_wrap = FlexWrap::NoWrap;
  JustifyContent justify_content = JustifyContent::FlexStart;
  AlignItems align_items = AlignItems::Stretch;
  AlignContent align_content = AlignContent::FlexStart;
  float gap = 0.0f;          // `gap` / `row-gap` / `column-gap` (single value)
  float flex_grow = 0.0f;
  float flex_shrink = 1.0f;
  Length flex_basis = Length::auto_value();

  // Grid container properties (explicit tracks only; auto-placement row by row).
  std::vector<GridTrack> grid_columns;
  std::vector<GridTrack> grid_rows;
  float row_gap = 0.0f;
  float column_gap = 0.0f;

  [[nodiscard]] bool is_flex_container() const noexcept { return display == Display::Flex; }
  [[nodiscard]] bool is_grid_container() const noexcept { return display == Display::Grid; }

  [[nodiscard]] float effective_line_height() const noexcept {
    return line_height > 0.0f ? line_height : font_size * 1.2f;
  }
};

/// One `property: value` pair. Values stay textual and are converted on demand:
/// keeps the parser small and makes "unsupported value" a quiet, per-property
/// failure instead of a parse error.
struct Declaration {
  std::string property;
  std::string value;
  bool important = false;
  std::size_t order = 0;  // document order across all sheets
};

struct MediaQuery {
  enum class MediaType { All, Screen, Print };
  MediaType type = MediaType::All;
  float min_width = -1.0f;
  float max_width = -1.0f;
  [[nodiscard]] bool matches(float viewport_width) const noexcept;
};

struct Rule {
  std::vector<dom::ComplexSelector> selectors;
  std::vector<Declaration> declarations;
  MediaQuery media;
};

struct FontFaceRule {
  std::string family;
  std::string source;  // as written, e.g. url(font.ttf)
};

/// Parses a declaration block ("color: red; margin: 4px") without surrounding braces.
/// Used for `style="..."` attributes and by `@font-face`.
[[nodiscard]] std::vector<Declaration> parse_declaration_block(std::string_view text,
                                                               Diagnostics* diagnostics = nullptr);

class StyleSheet {
 public:
  [[nodiscard]] static Result<StyleSheet> parse(std::string_view text, Diagnostics* diagnostics = nullptr);
  [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }
  [[nodiscard]] const std::vector<FontFaceRule>& font_faces() const noexcept { return font_faces_; }
  void append(StyleSheet&& other);

 private:
  std::vector<Rule> rules_;
  std::vector<FontFaceRule> font_faces_;
};

/// User-agent defaults, written as CSS and parsed by our own parser (self-hosting:
/// if the UA sheet renders, the CSS parser works).
[[nodiscard]] std::string_view user_agent_stylesheet();

/// Computed style per node index (index 0 = document node, which gets defaults).
struct StyleMap {
  std::vector<ComputedStyle> styles;  // indexed by dom::NodeId::index

  [[nodiscard]] const ComputedStyle& get(const dom::Document& document, dom::NodeId id) const;
};

[[nodiscard]] StyleMap compute_styles(const dom::Document& document, const StyleSheet& author_sheet,
                                      float viewport_width, Diagnostics* diagnostics = nullptr);

// Value helpers, exposed for tests and for the layout/paint modules.
[[nodiscard]] bool parse_color(std::string_view text, Color& out);
[[nodiscard]] bool parse_length(std::string_view text, Length& out);

/// Converts absolute units (px/pt/pc/in/cm/mm) to CSS pixels at 96 dpi.
/// Relative units (em/rem/%) and auto are returned unchanged.
[[nodiscard]] float absolute_length_to_px(const Length& length);
[[nodiscard]] std::string_view to_string(Display display) noexcept;

}  // namespace bonk::css
