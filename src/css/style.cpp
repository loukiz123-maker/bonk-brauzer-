#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <vector>

#include "internal.hpp"

namespace bonk::css {
namespace {

struct MatchedDeclaration {
  const Declaration* declaration = nullptr;
  int origin = 0;      // 0 = UA, 1 = author
  int important = 0;   // 1 when !important
  std::uint64_t specificity = 0;
  std::size_t order = 0;
};

bool cascade_less(const MatchedDeclaration& a, const MatchedDeclaration& b) {
  // Declaration order in CSS: origin+importance, then specificity, then source order.
  return std::tie(a.important, a.origin, a.specificity, a.order) <
         std::tie(b.important, b.origin, b.specificity, b.order);
}

/// Computed values for box properties have em/rem resolved; percentages stay
/// symbolic because they depend on the containing block, which only layout knows.
Length resolve_box_length(const Length& length, float font_size, float root_font_size) {
  switch (length.unit) {
    case Length::Unit::Em:
      return Length::px(length.value * font_size);
    case Length::Unit::Rem:
      return Length::px(length.value * root_font_size);
    default:
      return length;
  }
}

float resolve_font_relative(const Length& length, float parent_font_size, float root_font_size) {
  switch (length.unit) {
    case Length::Unit::Em:
      return length.value * parent_font_size;
    case Length::Unit::Rem:
      return length.value * root_font_size;
    case Length::Unit::Percent:
      return length.value * parent_font_size / 100.0f;
    default:
      return detail::absolute_to_px(length);
  }
}

bool parse_number(std::string_view text, float& out) {
  const std::string buffer(detail::trim_view(text));
  if (buffer.empty()) return false;
  char* end = nullptr;
  const double value = std::strtod(buffer.c_str(), &end);
  if (end == buffer.c_str() || *end != '\0') return false;
  out = static_cast<float>(value);
  return true;
}

int parse_font_weight(std::string_view text) {
  const std::string_view value = detail::trim_view(text);
  if (detail::equals_keyword(value, "normal")) return 400;
  if (detail::equals_keyword(value, "bold")) return 700;
  if (detail::equals_keyword(value, "bolder")) return 700;
  if (detail::equals_keyword(value, "lighter")) return 275;
  float number = 0.0f;
  if (!parse_number(value, number)) return 400;
  return static_cast<int>(std::clamp(number, 100.0f, 900.0f));
}

namespace {

/// `justify-content: space-between` etc. Unknown keywords fall back to flex-start.
JustifyContent parse_justify_content(std::string_view value) {
  if (detail::equals_keyword(value, "center")) return JustifyContent::Center;
  if (detail::equals_keyword(value, "flex-end") || detail::equals_keyword(value, "end")) return JustifyContent::FlexEnd;
  if (detail::equals_keyword(value, "space-between")) return JustifyContent::SpaceBetween;
  if (detail::equals_keyword(value, "space-around")) return JustifyContent::SpaceAround;
  if (detail::equals_keyword(value, "space-evenly")) return JustifyContent::SpaceEvenly;
  return JustifyContent::FlexStart;
}

AlignContent parse_align_content(std::string_view value) {
  if (detail::equals_keyword(value, "center")) return AlignContent::Center;
  if (detail::equals_keyword(value, "flex-end") || detail::equals_keyword(value, "end")) return AlignContent::FlexEnd;
  if (detail::equals_keyword(value, "stretch")) return AlignContent::Stretch;
  if (detail::equals_keyword(value, "space-between")) return AlignContent::SpaceBetween;
  if (detail::equals_keyword(value, "space-around")) return AlignContent::SpaceAround;
  if (detail::equals_keyword(value, "space-evenly")) return AlignContent::SpaceEvenly;
  return AlignContent::FlexStart;
}

/// Gap accepts a length without units only as 0 ("gap: 0").
bool parse_gap_value(std::string_view text, float& out) {
  const std::string_view word = detail::trim_view(text);
  if (word == "0") {
    out = 0.0f;
    return true;
  }
  css::Length length;
  if (!detail::parse_length_value(word, length)) return false;
  if (!length.is_absolute()) return false;  // percentage gaps need the container width; unsupported
  out = detail::absolute_to_px(length);
  return true;
}

bool parse_number_value(std::string_view text, float& out) {
  const std::string_view word = detail::trim_view(text);
  if (word.empty()) return false;
  // Keep the buffer alive: `end` points into it, so a temporary would dangle.
  const std::string text_copy(word);
  char* end = nullptr;
  const double value = std::strtod(text_copy.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<float>(value);
  return true;
}

/// "1fr 2fr 200px auto" / "repeat(3, 1fr)" -> track list.
std::vector<GridTrack> parse_grid_tracks(std::string_view value) {
  std::vector<GridTrack> tracks;
  const std::string_view text = detail::trim_view(value);
  if (text.empty() || detail::equals_keyword(text, "none")) return tracks;

  // repeat(N, <track-list>) — expanded before parsing the individual tracks.
  std::string expanded;
  std::string_view rest = text;
  while (!rest.empty()) {
    const std::size_t repeat_at = rest.find("repeat(");
    if (repeat_at == std::string_view::npos) {
      expanded.append(rest);
      break;
    }
    expanded.append(rest.substr(0, repeat_at));
    const std::size_t close = rest.find(')', repeat_at);
    if (close == std::string_view::npos) break;
    const std::string inner(rest.substr(repeat_at + 7, close - repeat_at - 7));
    const std::size_t comma = inner.find(',');
    if (comma == std::string::npos) break;
    const int count = std::atoi(inner.substr(0, comma).c_str());
    const std::string pattern = inner.substr(comma + 1);
    for (int i = 0; i < std::max(0, std::min(count, 32)); ++i) {
      expanded += ' ';
      expanded += pattern;
    }
    rest = rest.substr(close + 1);
  }

  for (const std::string_view piece : detail::split_top_level(expanded, ' ')) {
    const std::string_view word = detail::trim_view(piece);
    if (word.empty()) continue;
    GridTrack track;
    if (detail::equals_keyword(word, "auto")) {
      track.unit = GridTrack::Unit::Auto;
    } else if (word.size() > 2 && word.substr(word.size() - 2) == "fr") {
      track.unit = GridTrack::Unit::Fr;
      track.value = std::strtof(std::string(word.substr(0, word.size() - 2)).c_str(), nullptr);
    } else {
      css::Length length;
      if (!detail::parse_length_value(word, length)) continue;
      if (length.unit == css::Length::Unit::Percent) {
        track.unit = GridTrack::Unit::Percent;
        track.value = length.value;
      } else if (length.is_absolute()) {
        track.unit = GridTrack::Unit::Px;
        track.value = detail::absolute_to_px(length);
      } else {
        track.unit = GridTrack::Unit::Auto;
      }
    }
    tracks.push_back(track);
  }
  return tracks;
}

}  // namespace

class StyleComputer {
 public:
  StyleComputer(const dom::Document& document, float viewport_width, Diagnostics* diagnostics)
      : document_(document), diagnostics_(diagnostics) {
    if (const Result<StyleSheet> ua = StyleSheet::parse(user_agent_stylesheet(), diagnostics); ua.is_ok()) {
      ua_sheet_ = ua.value();
    }
    viewport_width_ = viewport_width;
  }

  void add_author_sheet(const StyleSheet& sheet) { author_sheet_ = &sheet; }

  StyleMap compute() {
    StyleMap map;
    map.styles.resize(document_.node_count());
    root_font_size_ = 16.0f;
    visit(document_.root(), ComputedStyle{}, map);
    return map;
  }

 private:
  void visit(dom::NodeId node, const ComputedStyle& parent_style, StyleMap& map) {
    ComputedStyle style = inherit_from(parent_style);
    if (document_.type(node) == dom::NodeType::Element) {
      if (document_.tag_name(node) == "html") root_font_size_ = style.font_size;
      apply_cascade(node, parent_style, style);
      if (document_.tag_name(node) == "html") root_font_size_ = style.font_size;
    }
    map.styles[node.index] = style;

    for (const dom::NodeId child : document_.children(node)) {
      if (document_.type(child) == dom::NodeType::Element) {
        visit(child, style, map);
      } else {
        // Text nodes inherit everything; layout only looks at element styles.
        ComputedStyle text_style = inherit_from(style);
        text_style.display = Display::Inline;
        map.styles[child.index] = text_style;
      }
    }
  }

  static ComputedStyle inherit_from(const ComputedStyle& parent) {
    ComputedStyle style;  // defaults are the initial values
    style.color = parent.color;
    style.font_size = parent.font_size;
    style.font_family = parent.font_family;
    style.font_weight = parent.font_weight;
    style.italic = parent.italic;
    style.line_height = parent.line_height;
    style.text_align = parent.text_align;
    style.white_space_pre = parent.white_space_pre;
    style.visible = parent.visible;
    style.background_color = Color{0, 0, 0, 0};
    return style;
  }

  void collect(const StyleSheet& sheet, dom::NodeId node, int origin, std::vector<MatchedDeclaration>& out) {
    for (const Rule& rule : sheet.rules()) {
      if (!rule.media.matches(viewport_width_)) continue;
      std::uint64_t best = 0;
      bool matched = false;
      for (const dom::ComplexSelector& selector : rule.selectors) {
        if (dom::matches(document_, node, selector)) {
          matched = true;
          best = std::max(best, selector.specificity.packed());
        }
      }
      if (!matched) continue;
      for (const Declaration& declaration : rule.declarations) {
        out.push_back(MatchedDeclaration{&declaration, origin, declaration.important ? 1 : 0, best,
                                         declaration.order});
      }
    }
  }

  void apply_cascade(dom::NodeId node, const ComputedStyle& parent_style, ComputedStyle& style) {
    std::vector<MatchedDeclaration> matched;
    collect(ua_sheet_, node, 0, matched);
    if (author_sheet_ != nullptr) collect(*author_sheet_, node, 1, matched);

    // `style="..."` wins over any selector (CSS declares it as the highest
    // specificity in the author origin).
    const std::string_view inline_style = document_.attribute(node, "style");
    if (!inline_style.empty()) {
      std::vector<Declaration> declarations = parse_declaration_block(inline_style, diagnostics_);
      for (const Declaration& declaration : declarations) {
        matched.push_back(
            MatchedDeclaration{&declaration, 2, declaration.important ? 1 : 0, ~0ull, declaration.order});
      }
      inline_declarations_.push_back(std::move(declarations));
    }

    std::stable_sort(matched.begin(), matched.end(), cascade_less);

    for (const MatchedDeclaration& entry : matched) {
      apply(*entry.declaration, parent_style, style);
    }
  }

  void apply(const Declaration& declaration, const ComputedStyle& parent_style, ComputedStyle& style) {
    const std::string& property = declaration.property;
    const std::string_view value = detail::trim_view(declaration.value);

    if (property == "display") {
      style.display = detail::display_from_keyword(value);
      return;
    }
    if (property == "position") {
      if (detail::equals_keyword(value, "static")) {
        style.position = Position::Static;
      } else if (detail::equals_keyword(value, "relative")) {
        style.position = Position::Relative;
      } else if (detail::equals_keyword(value, "absolute")) {
        style.position = Position::Absolute;
      } else if (detail::equals_keyword(value, "fixed")) {
        style.position = Position::Fixed;
      }
      // `sticky` and unknown keywords keep the previous value: no silent downgrade of
      // an author's `relative` when a later rule says `sticky`.
      return;
    }
    if (property == "top" || property == "right" || property == "bottom" || property == "left") {
      const int index = property == "top" ? 0 : property == "right" ? 1 : property == "bottom" ? 2 : 3;
      if (detail::equals_keyword(value, "auto")) {
        style.inset[index] = Length::auto_value();
        return;
      }
      Length length;
      if (detail::parse_length_value(value, length)) {
        style.inset[index] = resolve_box_length(length, style.font_size, root_font_size_);
      }
      return;
    }
    if (property == "z-index") {
      if (detail::equals_keyword(value, "auto")) {
        style.has_z_index = false;
        style.z_index = 0;
        return;
      }
      float number = 0.0f;
      if (parse_number(value, number)) {
        style.z_index = static_cast<int>(number);
        style.has_z_index = true;
      }
      return;
    }
    if (property == "color") {
      Color color;
      if (parse_color(value, color)) style.color = color;
      return;
    }
    if (property == "background" || property == "background-color") {
      Color color;
      // `background: #fff url(...)` — take the first color-looking component.
      if (parse_color(value, color)) {
        style.background_color = color;
        return;
      }
      for (const std::string_view piece : detail::split_top_level(value, ' ')) {
        if (parse_color(detail::trim_view(piece), color)) {
          style.background_color = color;
          return;
        }
      }
      return;
    }
    if (property == "font-size") {
      Length length;
      if (detail::parse_length_value(value, length)) {
        style.font_size = resolve_font_relative(length, parent_style.font_size, root_font_size_);
      } else if (detail::equals_keyword(value, "small")) {
        style.font_size = parent_style.font_size * 0.83f;
      } else if (detail::equals_keyword(value, "medium")) {
        style.font_size = 16.0f;
      } else if (detail::equals_keyword(value, "large")) {
        style.font_size = parent_style.font_size * 1.2f;
      } else if (detail::equals_keyword(value, "x-large")) {
        style.font_size = parent_style.font_size * 1.5f;
      }
      return;
    }
    if (property == "font-family") {
      style.font_family = std::string(value);
      return;
    }
    if (property == "font-weight") {
      style.font_weight = parse_font_weight(value);
      return;
    }
    if (property == "font-style") {
      style.italic = detail::equals_keyword(value, "italic") || detail::equals_keyword(value, "oblique");
      return;
    }
    if (property == "font") {
      // Very small subset: "<weight> <size>/<line-height> <family>" or "<size> <family>".
      for (const std::string_view piece : detail::split_top_level(value, ' ')) {
        const std::string_view token = detail::trim_view(piece);
        Length length;
        if (detail::parse_length_value(token, length)) {
          style.font_size = resolve_font_relative(length, parent_style.font_size, root_font_size_);
          continue;
        }
        if (detail::equals_keyword(token, "bold")) style.font_weight = 700;
        if (detail::equals_keyword(token, "italic")) style.italic = true;
      }
      return;
    }
    if (property == "line-height") {
      if (detail::equals_keyword(value, "normal")) {
        style.line_height = 0.0f;
        return;
      }
      float number = 0.0f;
      Length length;
      if (detail::parse_length_value(value, length)) {
        if (length.unit == Length::Unit::Em || length.unit == Length::Unit::Percent) {
          const float factor = length.unit == Length::Unit::Percent ? length.value / 100.0f : length.value;
          style.line_height = factor * style.font_size;
        } else {
          style.line_height = detail::absolute_to_px(length);
        }
      } else if (parse_number(value, number)) {
        style.line_height = number * style.font_size;  // unitless = multiplier
      }
      return;
    }
    if (property == "text-align") {
      if (detail::equals_keyword(value, "left") || detail::equals_keyword(value, "start")) {
        style.text_align = TextAlign::Left;
      } else if (detail::equals_keyword(value, "right") || detail::equals_keyword(value, "end")) {
        style.text_align = TextAlign::Right;
      } else if (detail::equals_keyword(value, "center")) {
        style.text_align = TextAlign::Center;
      } else if (detail::equals_keyword(value, "justify")) {
        style.text_align = TextAlign::Justify;
      }
      return;
    }
    if (property == "text-decoration" || property == "text-decoration-line") {
      style.text_decoration = value.find("underline") != std::string_view::npos ? TextDecoration::Underline
                                                                               : TextDecoration::None;
      return;
    }
    if (property == "white-space") {
      style.white_space_pre = detail::equals_keyword(value, "pre") || detail::equals_keyword(value, "pre-wrap") ||
                              detail::equals_keyword(value, "pre-line");
      return;
    }
    if (property == "visibility") {
      style.visible = !detail::equals_keyword(value, "hidden");
      return;
    }
    if (property == "opacity") {
      float number = 1.0f;
      if (parse_number(value, number)) style.opacity = std::clamp(number, 0.0f, 1.0f);
      return;
    }
    if (property == "margin") {
      Length box[4];
      if (detail::parse_box_shorthand(value, box)) {
        for (int i = 0; i < 4; ++i) style.margin[i] = resolve_box_length(box[i], style.font_size, root_font_size_);
      }
      return;
    }
    if (property == "padding") {
      Length box[4];
      if (detail::parse_box_shorthand(value, box)) {
        for (int i = 0; i < 4; ++i) style.padding[i] = resolve_box_length(box[i], style.font_size, root_font_size_);
      }
      return;
    }
    if (property.rfind("margin-", 0) == 0 || property.rfind("padding-", 0) == 0) {
      const bool margin = property.rfind("margin-", 0) == 0;
      const std::string_view side = std::string_view(property).substr(margin ? 7 : 8);
      int index = -1;
      if (side == "top") index = 0;
      if (side == "right") index = 1;
      if (side == "bottom") index = 2;
      if (side == "left") index = 3;
      if (index < 0) return;
      Length length;
      if (detail::parse_length_value(value, length)) {
        (margin ? style.margin : style.padding)[index] = resolve_box_length(length, style.font_size, root_font_size_);
      }
      return;
    }
    if (property == "border" || property.rfind("border-", 0) == 0) {
      // width | style | color in any order; "none"/"hidden" zero the width.
      float width = 3.0f;  // medium
      bool has_width = false;
      Color color;
      bool has_color = false;
      bool hidden = false;
      for (const std::string_view piece : detail::split_top_level(value, ' ')) {
        const std::string_view token = detail::trim_view(piece);
        if (token.empty()) continue;
        Length length;
        if (detail::parse_length_value(token, length)) {
          width = detail::absolute_to_px(length);
          has_width = true;
          continue;
        }
        if (parse_color(token, color)) {
          has_color = true;
          continue;
        }
        if (detail::equals_keyword(token, "none") || detail::equals_keyword(token, "hidden")) hidden = true;
        if (detail::equals_keyword(token, "thin")) {
          width = 1.0f;
          has_width = true;
        }
        if (detail::equals_keyword(token, "medium")) {
          width = 3.0f;
          has_width = true;
        }
        if (detail::equals_keyword(token, "thick")) {
          width = 5.0f;
          has_width = true;
        }
      }
      if (!has_width) width = 3.0f;
      if (hidden) width = 0.0f;
      if (has_color) style.border_color = color;

      int first = 0;
      int last = 3;
      if (property != "border") {
        const std::string_view side = std::string_view(property).substr(7);
        if (side == "top") {
          first = last = 0;
        } else if (side == "right") {
          first = last = 1;
        } else if (side == "bottom") {
          first = last = 2;
        } else if (side == "left") {
          first = last = 3;
        } else if (side == "width") {
          for (int i = 0; i < 4; ++i) style.border_width[i] = width;
          return;
        } else if (side == "color") {
          if (has_color) style.border_color = color;
          return;
        } else {
          return;
        }
      }
      for (int i = first; i <= last; ++i) style.border_width[i] = width;
      if (has_color && style.border_color.a == 0) style.border_color = color;
      return;
    }
    if (property == "border-width") {
      Length box[4];
      if (detail::parse_box_shorthand(value, box)) {
        for (int i = 0; i < 4; ++i) style.border_width[i] = detail::absolute_to_px(box[i]);
      }
      return;
    }
    if (property == "border-color") {
      Color color;
      if (parse_color(value, color)) style.border_color = color;
      return;
    }
    if (property == "width" || property == "height" || property == "min-height" || property == "min-width" ||
        property == "max-width") {
      Length length;
      if (!detail::parse_length_value(value, length)) return;
      if (property == "width") {
        style.width = length;
      } else if (property == "height") {
        style.height = length;
      } else if (property == "min-height") {
        style.min_height = detail::absolute_to_px(length);
      } else if (property == "min-width") {
        style.min_width = detail::absolute_to_px(length);
      } else if (property == "max-width") {
        style.max_width = detail::absolute_to_px(length);
      }
      return;
    }
    if (property == "list-style" || property == "list-style-type") {
      style.list_item = true;
      return;
    }
    // --- flexbox -----------------------------------------------------------
    if (property == "flex-direction") {
      if (detail::equals_keyword(value, "row")) style.flex_direction = FlexDirection::Row;
      else if (detail::equals_keyword(value, "row-reverse")) style.flex_direction = FlexDirection::RowReverse;
      else if (detail::equals_keyword(value, "column")) style.flex_direction = FlexDirection::Column;
      else if (detail::equals_keyword(value, "column-reverse")) style.flex_direction = FlexDirection::ColumnReverse;
      return;
    }
    if (property == "flex-wrap") {
      if (detail::equals_keyword(value, "nowrap")) style.flex_wrap = FlexWrap::NoWrap;
      else if (detail::equals_keyword(value, "wrap")) style.flex_wrap = FlexWrap::Wrap;
      else if (detail::equals_keyword(value, "wrap-reverse")) style.flex_wrap = FlexWrap::WrapReverse;
      return;
    }
    if (property == "flex-flow") {
      for (const std::string_view piece : detail::split_top_level(value, ' ')) {
        const std::string_view word = detail::trim_view(piece);
        if (word.empty()) continue;
        if (detail::equals_keyword(word, "row")) style.flex_direction = FlexDirection::Row;
        else if (detail::equals_keyword(word, "row-reverse")) style.flex_direction = FlexDirection::RowReverse;
        else if (detail::equals_keyword(word, "column")) style.flex_direction = FlexDirection::Column;
        else if (detail::equals_keyword(word, "column-reverse")) style.flex_direction = FlexDirection::ColumnReverse;
        else if (detail::equals_keyword(word, "wrap")) style.flex_wrap = FlexWrap::Wrap;
        else if (detail::equals_keyword(word, "nowrap")) style.flex_wrap = FlexWrap::NoWrap;
      }
      return;
    }
    if (property == "justify-content") {
      style.justify_content = parse_justify_content(value);
      return;
    }
    if (property == "align-items") {
      if (detail::equals_keyword(value, "stretch")) style.align_items = AlignItems::Stretch;
      else if (detail::equals_keyword(value, "flex-start") || detail::equals_keyword(value, "start")) style.align_items = AlignItems::FlexStart;
      else if (detail::equals_keyword(value, "center")) style.align_items = AlignItems::Center;
      else if (detail::equals_keyword(value, "flex-end") || detail::equals_keyword(value, "end")) style.align_items = AlignItems::FlexEnd;
      return;
    }
    if (property == "align-content") {
      style.align_content = parse_align_content(value);
      return;
    }
    if (property == "gap" || property == "grid-gap") {
      std::vector<std::string_view> pieces = detail::split_top_level(value, ' ');
      float first = 0.0f;
      float second = 0.0f;
      if (parse_gap_value(pieces.empty() ? value : pieces[0], first)) {
        second = first;
        if (pieces.size() > 1) (void)parse_gap_value(pieces[1], second);
        style.row_gap = first;
        style.column_gap = second;
        style.gap = first;  // flex containers use one value
      }
      return;
    }
    if (property == "row-gap" || property == "grid-row-gap") {
      float gap_px = 0.0f;
      if (parse_gap_value(value, gap_px)) style.row_gap = gap_px;
      return;
    }
    if (property == "column-gap" || property == "grid-column-gap") {
      float gap_px = 0.0f;
      if (parse_gap_value(value, gap_px)) {
        style.column_gap = gap_px;
        style.gap = gap_px;
      }
      return;
    }
    if (property == "flex-grow") {
      float grow = 0.0f;
      if (parse_number_value(value, grow)) style.flex_grow = std::max(0.0f, grow);
      return;
    }
    if (property == "flex-shrink") {
      float shrink = 1.0f;
      if (parse_number_value(value, shrink)) style.flex_shrink = std::max(0.0f, shrink);
      return;
    }
    if (property == "flex-basis") {
      Length basis;
      if (detail::equals_keyword(value, "auto")) {
        style.flex_basis = Length::auto_value();
      } else if (detail::parse_length_value(value, basis)) {
        style.flex_basis = basis;
      }
      return;
    }
    if (property == "flex") {
      // `flex: <grow> [<shrink>] [<basis>]`, `flex: auto`, `flex: none`, `flex: 1 1 0%`.
      if (detail::equals_keyword(value, "auto")) {
        style.flex_grow = 1.0f;
        style.flex_shrink = 1.0f;
        style.flex_basis = Length::auto_value();
      } else if (detail::equals_keyword(value, "none")) {
        style.flex_grow = 0.0f;
        style.flex_shrink = 0.0f;
        style.flex_basis = Length::auto_value();
      } else {
        std::vector<std::string_view> pieces = detail::split_top_level(value, ' ');
        std::size_t number_count = 0;
        for (const std::string_view piece : pieces) {
          const std::string_view word = detail::trim_view(piece);
          if (word.empty()) continue;
          float number = 0.0f;
          if (number_count < 2 && parse_number_value(word, number)) {
            if (number_count == 0) style.flex_grow = std::max(0.0f, number);
            else style.flex_shrink = std::max(0.0f, number);
            ++number_count;
            continue;
          }
          Length basis;
          if (detail::equals_keyword(word, "auto")) style.flex_basis = Length::auto_value();
          else if (detail::parse_length_value(word, basis)) style.flex_basis = basis;
        }
      }
      return;
    }
    // --- grid --------------------------------------------------------------
    if (property == "grid-template-columns" || property == "grid-template-rows") {
      std::vector<GridTrack> tracks = parse_grid_tracks(value);
      if (property == "grid-template-columns") style.grid_columns = std::move(tracks);
      else style.grid_rows = std::move(tracks);
      return;
    }
    // Unknown or unsupported property: silently ignored, exactly like a browser
    // ignoring a declaration it does not understand.
  }

  const dom::Document& document_;
  Diagnostics* diagnostics_ = nullptr;
  StyleSheet ua_sheet_;
  const StyleSheet* author_sheet_ = nullptr;
  // Keeps inline declarations alive while `matched` holds pointers into them.
  std::vector<std::vector<Declaration>> inline_declarations_;
  float viewport_width_ = 1024.0f;
  float root_font_size_ = 16.0f;
};

}  // namespace

const ComputedStyle& StyleMap::get(const dom::Document& document, dom::NodeId id) const {
  static const ComputedStyle kFallback{};
  if (id.index >= styles.size()) return kFallback;
  (void)document;
  return styles[id.index];
}

StyleMap compute_styles(const dom::Document& document, const StyleSheet& author_sheet, float viewport_width,
                        Diagnostics* diagnostics) {
  StyleComputer computer(document, viewport_width, diagnostics);
  computer.add_author_sheet(author_sheet);
  return computer.compute();
}

}  // namespace bonk::css
