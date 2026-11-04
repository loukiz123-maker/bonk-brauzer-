#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bonk/core/strings.hpp"
#include "internal.hpp"

namespace bonk::css {
namespace detail {
namespace {

struct NamedColor {
  std::string_view name;
  Color color;
};

constexpr NamedColor kNamedColors[] = {
    {"black", {0, 0, 0, 255}},          {"white", {255, 255, 255, 255}},
    {"red", {255, 0, 0, 255}},          {"green", {0, 128, 0, 255}},
    {"lime", {0, 255, 0, 255}},         {"blue", {0, 0, 255, 255}},
    {"yellow", {255, 255, 0, 255}},     {"cyan", {0, 255, 255, 255}},
    {"aqua", {0, 255, 255, 255}},       {"magenta", {255, 0, 255, 255}},
    {"fuchsia", {255, 0, 255, 255}},    {"silver", {192, 192, 192, 255}},
    {"gray", {128, 128, 128, 255}},     {"grey", {128, 128, 128, 255}},
    {"maroon", {128, 0, 0, 255}},       {"olive", {128, 128, 0, 255}},
    {"purple", {128, 0, 128, 255}},     {"teal", {0, 128, 128, 255}},
    {"navy", {0, 0, 128, 255}},         {"orange", {255, 165, 0, 255}},
    {"brown", {165, 42, 42, 255}},      {"pink", {255, 192, 203, 255}},
    {"gold", {255, 215, 0, 255}},       {"violet", {238, 130, 238, 255}},
    {"indigo", {75, 0, 130, 255}},      {"beige", {245, 245, 220, 255}},
    {"ivory", {255, 255, 240, 255}},    {"khaki", {240, 230, 140, 255}},
    {"salmon", {250, 128, 114, 255}},   {"coral", {255, 127, 80, 255}},
    {"crimson", {220, 20, 60, 255}},    {"darkred", {139, 0, 0, 255}},
    {"darkgreen", {0, 100, 0, 255}},    {"darkblue", {0, 0, 139, 255}},
    {"lightgray", {211, 211, 211, 255}}, {"lightgrey", {211, 211, 211, 255}},
    {"darkgray", {169, 169, 169, 255}}, {"darkgrey", {169, 169, 169, 255}},
    {"dimgray", {105, 105, 105, 255}},  {"dimgrey", {105, 105, 105, 255}},
    {"whitesmoke", {245, 245, 245, 255}}, {"gainsboro", {220, 220, 220, 255}},
    {"aliceblue", {240, 248, 255, 255}}, {"steelblue", {70, 130, 180, 255}},
    {"skyblue", {135, 206, 235, 255}},  {"royalblue", {65, 105, 225, 255}},
    {"tomato", {255, 99, 71, 255}},     {"orchid", {218, 112, 214, 255}},
    {"plum", {221, 160, 221, 255}},     {"turquoise", {64, 224, 208, 255}},
    {"transparent", {0, 0, 0, 0}},
};

bool parse_hex_digit(char c, int& value) {
  if (c >= '0' && c <= '9') {
    value = c - '0';
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    value = c - 'a' + 10;
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    value = c - 'A' + 10;
    return true;
  }
  return false;
}

bool parse_float(std::string_view text, float& out) {
  if (text.empty()) return false;
  const std::string buffer(text);
  char* end = nullptr;
  const double value = std::strtod(buffer.c_str(), &end);
  if (end == buffer.c_str() || *end != '\0' || !std::isfinite(value)) return false;
  out = static_cast<float>(value);
  return true;
}

std::string_view strip_comments(std::string_view text, std::string& storage) {
  storage.clear();
  storage.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, 2, "/*") == 0) {
      const std::size_t end = text.find("*/", i + 2);
      if (end == std::string_view::npos) break;
      i = end + 2;
      continue;
    }
    storage.push_back(text[i]);
    ++i;
  }
  return storage;
}

/// Reads the prelude up to `stop` at nesting depth 0, returning the prelude and
/// leaving `position` on the stop character.
std::string_view read_prelude(std::string_view text, std::size_t& position, char stop) {
  const std::size_t start = position;
  int parens = 0;
  char quote = 0;
  while (position < text.size()) {
    const char c = text[position];
    if (quote != 0) {
      if (c == quote) quote = 0;
      ++position;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++parens;
    } else if (c == ')') {
      --parens;
    } else if (c == stop && parens <= 0) {
      return text.substr(start, position - start);
    }
    ++position;
  }
  return text.substr(start);
}

/// Reads a `{ ... }` block; `position` must point at '{'.
std::string_view read_block(std::string_view text, std::size_t& position) {
  if (position >= text.size() || text[position] != '{') return {};
  const std::size_t start = ++position;
  int depth = 1;
  char quote = 0;
  while (position < text.size()) {
    const char c = text[position];
    if (quote != 0) {
      if (c == quote) quote = 0;
      ++position;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        const std::string_view block = text.substr(start, position - start);
        ++position;
        return block;
      }
    }
    ++position;
  }
  return text.substr(start);
}

MediaQuery parse_media_query(std::string_view text) {
  MediaQuery query;
  const std::string lowered = to_ascii_lower(text);
  if (lowered.find("print") != std::string::npos && lowered.find("screen") == std::string::npos) {
    query.type = MediaQuery::MediaType::Print;
  } else if (lowered.find("screen") != std::string::npos) {
    query.type = MediaQuery::MediaType::Screen;
  }
  const std::size_t min_pos = lowered.find("min-width");
  if (min_pos != std::string::npos) {
    const std::size_t colon = lowered.find(':', min_pos);
    const std::size_t close = lowered.find(')', colon);
    if (colon != std::string::npos && close != std::string::npos) {
      Length length;
      if (parse_length_value(trim_view(std::string_view(lowered).substr(colon + 1, close - colon - 1)), length)) {
        query.min_width = absolute_to_px(length);
      }
    }
  }
  const std::size_t max_pos = lowered.find("max-width");
  if (max_pos != std::string::npos) {
    const std::size_t colon = lowered.find(':', max_pos);
    const std::size_t close = lowered.find(')', colon);
    if (colon != std::string::npos && close != std::string::npos) {
      Length length;
      if (parse_length_value(trim_view(std::string_view(lowered).substr(colon + 1, close - colon - 1)), length)) {
        query.max_width = absolute_to_px(length);
      }
    }
  }
  return query;
}

void parse_declarations(std::string_view block, std::vector<Declaration>& out, Diagnostics* diagnostics,
                        std::size_t& order) {
  for (const std::string_view chunk : split_top_level(block, ';')) {
    const std::string_view declaration = trim_view(chunk);
    if (declaration.empty()) continue;
    const std::size_t colon = declaration.find(':');
    if (colon == std::string_view::npos) {
      if (diagnostics != nullptr) {
        diagnostics->add(Severity::Warning, SourcePos{}, "css.invalid_declaration: missing ':' in '" +
                                                            std::string(declaration) + "'");
      }
      continue;
    }
    Declaration parsed;
    parsed.property = to_ascii_lower(trim_view(declaration.substr(0, colon)));
    parsed.value = std::string(trim_view(declaration.substr(colon + 1)));
    if (parsed.property.empty() || parsed.value.empty()) continue;

    const std::string lowered_value = to_ascii_lower(parsed.value);
    const std::size_t important = lowered_value.rfind("!important");
    if (important != std::string::npos) {
      const std::string_view tail = trim_view(std::string_view(lowered_value).substr(important));
      if (tail == "!important") {
        parsed.important = true;
        parsed.value = std::string(trim_right(std::string_view(parsed.value).substr(0, important)));
      }
    }
    parsed.order = order++;
    out.push_back(std::move(parsed));
  }
}

}  // namespace

std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  int depth = 0;
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (c == separator && depth == 0) {
      parts.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(text.substr(start));
  return parts;
}

std::string_view trim_view(std::string_view text) { return trim(text); }

float absolute_to_px(const Length& length) {
  switch (length.unit) {
    case Length::Unit::Pt:
      return length.value * 96.0f / 72.0f;
    case Length::Unit::Pc:
      return length.value * 16.0f;  // 1pc = 12pt
    case Length::Unit::In:
      return length.value * 96.0f;
    case Length::Unit::Cm:
      return length.value * 96.0f / 2.54f;
    case Length::Unit::Mm:
      return length.value * 96.0f / 25.4f;
    default:
      return length.value;
  }
}

bool parse_length_value(std::string_view text, Length& out) {
  const std::string_view value = trim_view(text);
  if (value.empty()) return false;
  if (equals_keyword(value, "auto")) {
    out = Length::auto_value();
    return true;
  }
  if (equals_keyword(value, "normal")) return false;

  std::size_t number_end = 0;
  if (value[number_end] == '+' || value[number_end] == '-') ++number_end;
  bool seen_digit = false;
  while (number_end < value.size() &&
         ((value[number_end] >= '0' && value[number_end] <= '9') || value[number_end] == '.')) {
    if (value[number_end] >= '0' && value[number_end] <= '9') seen_digit = true;
    ++number_end;
  }
  if (!seen_digit) return false;

  float number = 0.0f;
  if (!parse_float(value.substr(0, number_end), number)) return false;

  const std::string_view unit = trim_view(value.substr(number_end));
  if (unit.empty()) {
    out = Length{number, Length::Unit::Px};  // unitless: only 0 is legal, and 0px == 0
    return number == 0.0f;
  }
  if (equals_keyword(unit, "px")) {
    out = Length{number, Length::Unit::Px};
  } else if (equals_keyword(unit, "pt")) {
    out = Length{number, Length::Unit::Pt};
  } else if (equals_keyword(unit, "pc")) {
    out = Length{number, Length::Unit::Pc};
  } else if (equals_keyword(unit, "in")) {
    out = Length{number, Length::Unit::In};
  } else if (equals_keyword(unit, "cm")) {
    out = Length{number, Length::Unit::Cm};
  } else if (equals_keyword(unit, "mm")) {
    out = Length{number, Length::Unit::Mm};
  } else if (equals_keyword(unit, "em")) {
    out = Length{number, Length::Unit::Em};
  } else if (equals_keyword(unit, "rem")) {
    out = Length{number, Length::Unit::Rem};
  } else if (unit == "%") {
    out = Length{number, Length::Unit::Percent};
  } else {
    return false;
  }
  return true;
}

bool parse_box_shorthand(std::string_view text, Length out[4]) {
  const std::vector<std::string_view> parts = split_top_level(text, ' ');
  std::vector<Length> lengths;
  for (const std::string_view part : parts) {
    const std::string_view piece = trim_view(part);
    if (piece.empty()) continue;
    Length length;
    if (!parse_length_value(piece, length)) return false;
    lengths.push_back(length);
  }
  switch (lengths.size()) {
    case 1:
      out[0] = out[1] = out[2] = out[3] = lengths[0];
      return true;
    case 2:
      out[0] = out[2] = lengths[0];
      out[1] = out[3] = lengths[1];
      return true;
    case 3:
      out[0] = lengths[0];
      out[1] = out[3] = lengths[1];
      out[2] = lengths[2];
      return true;
    case 4:
      out[0] = lengths[0];
      out[1] = lengths[1];
      out[2] = lengths[2];
      out[3] = lengths[3];
      return true;
    default:
      return false;
  }
}

bool equals_keyword(std::string_view text, std::string_view keyword) {
  return equals_ci(trim_view(text), keyword);
}

Display display_from_keyword(std::string_view keyword) noexcept {
  if (equals_ci(keyword, "block")) return Display::Block;
  if (equals_ci(keyword, "inline")) return Display::Inline;
  if (equals_ci(keyword, "inline-block")) return Display::InlineBlock;
  if (equals_ci(keyword, "flex")) return Display::Flex;
  if (equals_ci(keyword, "inline-flex")) return Display::Flex;
  if (equals_ci(keyword, "grid")) return Display::Grid;
  if (equals_ci(keyword, "inline-grid")) return Display::Grid;
  if (equals_ci(keyword, "none")) return Display::None;
  if (equals_ci(keyword, "list-item")) return Display::ListItem;
  if (equals_ci(keyword, "table")) return Display::Table;
  if (equals_ci(keyword, "table-row")) return Display::TableRow;
  if (equals_ci(keyword, "table-cell")) return Display::TableCell;
  return Display::Inline;
}

}  // namespace detail

bool MediaQuery::matches(float viewport_width) const noexcept {
  if (type == MediaType::Print) return false;  // we are a screen browser
  if (min_width >= 0.0f && viewport_width < min_width) return false;
  if (max_width >= 0.0f && viewport_width > max_width) return false;
  return true;
}

std::string Color::to_string() const {
  std::string out = "rgba(";
  out += std::to_string(r);
  out += ", ";
  out += std::to_string(g);
  out += ", ";
  out += std::to_string(b);
  out += ", ";
  out += std::to_string(static_cast<double>(a) / 255.0);
  out += ')';
  return out;
}

std::string Length::to_string() const {
  if (unit == Unit::Auto) return "auto";
  std::string out;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
  out = buffer;
  switch (unit) {
    case Unit::Px:
      out += "px";
      break;
    case Unit::Percent:
      out += '%';
      break;
    case Unit::Em:
      out += "em";
      break;
    case Unit::Rem:
      out += "rem";
      break;
    case Unit::Pt:
      out += "pt";
      break;
    case Unit::Cm:
      out += "cm";
      break;
    case Unit::Mm:
      out += "mm";
      break;
    case Unit::In:
      out += "in";
      break;
    case Unit::Pc:
      out += "pc";
      break;
    case Unit::Auto:
      break;
  }
  return out;
}

bool parse_color(std::string_view text, Color& out) {
  const std::string_view value = detail::trim_view(text);
  if (value.empty()) return false;

  if (value[0] == '#') {
    const std::string_view hex = value.substr(1);
    int digits[8] = {0};
    for (std::size_t i = 0; i < hex.size(); ++i) {
      if (!detail::parse_hex_digit(hex[i], digits[i])) return false;
    }
    switch (hex.size()) {
      case 3:
        out = Color{static_cast<std::uint8_t>(digits[0] * 17), static_cast<std::uint8_t>(digits[1] * 17),
                    static_cast<std::uint8_t>(digits[2] * 17), 255};
        return true;
      case 6:
        out = Color{static_cast<std::uint8_t>(digits[0] * 16 + digits[1]),
                    static_cast<std::uint8_t>(digits[2] * 16 + digits[3]),
                    static_cast<std::uint8_t>(digits[4] * 16 + digits[5]), 255};
        return true;
      case 8:
        out = Color{static_cast<std::uint8_t>(digits[0] * 16 + digits[1]),
                    static_cast<std::uint8_t>(digits[2] * 16 + digits[3]),
                    static_cast<std::uint8_t>(digits[4] * 16 + digits[5]),
                    static_cast<std::uint8_t>(digits[6] * 16 + digits[7])};
        return true;
      default:
        return false;
    }
  }

  const std::string lowered = to_ascii_lower(value);
  if (lowered.rfind("rgb", 0) == 0) {
    const std::size_t open = lowered.find('(');
    const std::size_t close = lowered.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    const std::vector<std::string_view> parts =
        detail::split_top_level(std::string_view(lowered).substr(open + 1, close - open - 1), ',');
    if (parts.size() != 3 && parts.size() != 4) return false;
    auto channel = [](std::string_view piece, int& result) {
      float number = 0.0f;
      if (!detail::parse_float(detail::trim_view(piece), number)) return false;
      result = static_cast<int>(std::lround(number));
      return result >= 0 && result <= 255;
    };
    int r = 0;
    int g = 0;
    int b = 0;
    if (!channel(parts[0], r) || !channel(parts[1], g) || !channel(parts[2], b)) return false;
    int alpha = 255;
    if (parts.size() == 4) {
      float alpha_value = 0.0f;
      if (!detail::parse_float(detail::trim_view(parts[3]), alpha_value)) return false;
      alpha = static_cast<int>(std::lround(alpha_value * 255.0f));
      if (alpha < 0) alpha = 0;
      if (alpha > 255) alpha = 255;
    }
    out = Color{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b),
                static_cast<std::uint8_t>(alpha)};
    return true;
  }

  for (const detail::NamedColor& named : detail::kNamedColors) {
    if (named.name == lowered) {
      out = named.color;
      return true;
    }
  }
  return false;
}

bool parse_length(std::string_view text, Length& out) { return detail::parse_length_value(text, out); }

float absolute_length_to_px(const Length& length) { return detail::absolute_to_px(length); }

std::string_view to_string(Display display) noexcept {
  switch (display) {
    case Display::Inline:
      return "inline";
    case Display::Block:
      return "block";
    case Display::InlineBlock:
      return "inline-block";
    case Display::Flex:
      return "flex";
    case Display::Grid:
      return "grid";
    case Display::None:
      return "none";
    case Display::ListItem:
      return "list-item";
    case Display::Table:
      return "table";
    case Display::TableRow:
      return "table-row";
    case Display::TableCell:
      return "table-cell";
  }
  return "inline";
}

Result<StyleSheet> StyleSheet::parse(std::string_view text, Diagnostics* diagnostics) {
  StyleSheet sheet;
  std::string comment_storage;
  const std::string_view cleaned = detail::strip_comments(text, comment_storage);

  std::size_t position = 0;
  std::size_t order = 0;

  while (position < cleaned.size()) {
    while (position < cleaned.size() && std::isspace(static_cast<unsigned char>(cleaned[position]))) ++position;
    if (position >= cleaned.size()) break;

    if (cleaned[position] == '}') {  // tolerate a stray closing brace
      ++position;
      continue;
    }

    if (cleaned[position] == '@') {
      const std::string_view prelude = detail::read_prelude(cleaned, position, '{');
      std::string_view prelude_text = detail::trim_view(prelude);
      // A prelude that runs into ';' is a statement at-rule (@charset, @import): skip it.
      if (position >= cleaned.size() || cleaned[position] != '{') {
        if (position < cleaned.size() && cleaned[position] == ';') ++position;
        continue;
      }
      const std::string_view block = detail::read_block(cleaned, position);
      const std::string lowered = to_ascii_lower(prelude_text);

      if (lowered.rfind("@media", 0) == 0) {
        const MediaQuery query = detail::parse_media_query(std::string_view(lowered).substr(6));
        StyleSheet nested;
        // Parse the nested block as a sheet, then inherit the media query.
        Result<StyleSheet> inner = StyleSheet::parse(block, diagnostics);
        if (inner.is_ok()) {
          for (Rule rule : inner.value().rules()) {
            rule.media = query;
            for (Declaration& declaration : rule.declarations) declaration.order = order++;
            sheet.rules_.push_back(std::move(rule));
          }
        }
      } else if (lowered.rfind("@font-face", 0) == 0) {
        FontFaceRule font_face;
        std::vector<Declaration> declarations;
        detail::parse_declarations(block, declarations, diagnostics, order);
        for (const Declaration& declaration : declarations) {
          if (declaration.property == "font-family") {
            font_face.family = std::string(detail::trim_view(declaration.value));
            if (!font_face.family.empty() && (font_face.family.front() == '"' || font_face.family.front() == '\'')) {
              font_face.family = font_face.family.substr(1, font_face.family.size() - 2);
            }
          } else if (declaration.property == "src") {
            font_face.source = declaration.value;
          }
        }
        if (!font_face.family.empty()) sheet.font_faces_.push_back(std::move(font_face));
      } else {
        if (diagnostics != nullptr) {
          diagnostics->add(Severity::Hint, SourcePos{},
                           "css.unsupported_at_rule: " + std::string(prelude_text.substr(0, 40)));
        }
      }
      continue;
    }

    const std::string_view selectors_text = detail::read_prelude(cleaned, position, '{');
    if (position >= cleaned.size() || cleaned[position] != '{') break;  // unterminated rule
    const std::string_view block = detail::read_block(cleaned, position);

    Result<std::vector<dom::ComplexSelector>> selectors =
        dom::parse_selector_list(detail::trim_view(selectors_text));
    if (selectors.is_error()) {
      if (diagnostics != nullptr) {
        diagnostics->add(Severity::Warning, SourcePos{},
                         "css.invalid_selector: " + selectors.error().message + " in \"" +
                             std::string(detail::trim_view(selectors_text)) + "\"");
      }
      continue;
    }
    Rule rule;
    rule.selectors = std::move(selectors.value());
    detail::parse_declarations(block, rule.declarations, diagnostics, order);
    sheet.rules_.push_back(std::move(rule));
  }

  return Result<StyleSheet>::success(std::move(sheet));
}

std::vector<Declaration> parse_declaration_block(std::string_view text, Diagnostics* diagnostics) {
  std::vector<Declaration> declarations;
  std::size_t order = 0;
  detail::parse_declarations(text, declarations, diagnostics, order);
  return declarations;
}

void StyleSheet::append(StyleSheet&& other) {
  for (Rule& rule : other.rules_) rules_.push_back(std::move(rule));
  for (FontFaceRule& font_face : other.font_faces_) font_faces_.push_back(std::move(font_face));
  other.rules_.clear();
  other.font_faces_.clear();
}

}  // namespace bonk::css
