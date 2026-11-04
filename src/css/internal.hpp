// Internal helpers shared by the CSS implementation files.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "bonk/css/css.hpp"

namespace bonk::css::detail {

/// Splits `text` on top-level `separator` (ignoring separators inside quotes,
/// parentheses and brackets). Empty fields are dropped.
[[nodiscard]] std::vector<std::string_view> split_top_level(std::string_view text, char separator);

[[nodiscard]] std::string_view trim_view(std::string_view text);

/// "12px" -> Length; "auto" -> Auto. Returns false for anything else.
[[nodiscard]] bool parse_length_value(std::string_view text, Length& out);

/// Converts an absolute length to pixels (1in = 96px, 1pt = 96/72px).
[[nodiscard]] float absolute_to_px(const Length& length);

/// Expands `margin`/`padding` (1..4 values) into four Lengths. Returns false when
/// any component is not a length.
[[nodiscard]] bool parse_box_shorthand(std::string_view text, Length out[4]);

/// Case-insensitive keyword comparison.
[[nodiscard]] bool equals_keyword(std::string_view text, std::string_view keyword);

[[nodiscard]] Display display_from_keyword(std::string_view keyword) noexcept;

}  // namespace bonk::css::detail
