// Bonk Brauzer — small ASCII/byte string utilities.
//
// Everything here operates on bytes but treats ASCII specially, which is
// exactly the split the HTML/CSS/URL specs require (ASCII case-insensitivity
// for tag names and keywords, byte-wise comparison otherwise). Full Unicode
// case folding belongs to the text module and is not needed at this layer.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bonk {

/// WHATWG "strip leading and trailing ASCII whitespace".
[[nodiscard]] std::string_view trim(std::string_view input) noexcept;
[[nodiscard]] std::string_view trim_left(std::string_view input) noexcept;
[[nodiscard]] std::string_view trim_right(std::string_view input) noexcept;

/// Split on a single byte delimiter. Empty input yields zero fields;
/// "a,,b" yields {"a", "", "b"}.
[[nodiscard]] std::vector<std::string_view> split(std::string_view input, char delimiter);
[[nodiscard]] std::string join(const std::vector<std::string_view>& parts, std::string_view separator);

[[nodiscard]] bool starts_with_ci(std::string_view haystack, std::string_view prefix) noexcept;
[[nodiscard]] bool equals_ci(std::string_view a, std::string_view b) noexcept;

[[nodiscard]] std::string to_ascii_lower(std::string_view input);
[[nodiscard]] std::string to_ascii_upper(std::string_view input);

/// Replace every occurrence of `needle` (must be non-empty) with `replacement`.
[[nodiscard]] std::string replace_all(std::string_view input, std::string_view needle, std::string_view replacement);

/// Left/right pad to `width` printing columns (ASCII assumption).
[[nodiscard]] std::string pad_right(std::string_view input, std::size_t width);
[[nodiscard]] std::string pad_left(std::string_view input, std::size_t width);

/// Parse a non-negative decimal integer from an ASCII byte range.
/// Returns false on empty input, overflow, or any non-digit byte.
[[nodiscard]] bool parse_u64(std::string_view input, unsigned long long& out) noexcept;

}  // namespace bonk
