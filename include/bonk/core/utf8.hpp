// Bonk Brauzer — UTF-8 primitives.
//
// The whole engine stores text as UTF-8 `std::string`, mirroring the WHATWG
// decision ("the DOM string type is a sequence of code points held as
// USVString"). Decoding is *lenient* like the WHATWG Encoding Standard:
// an invalid or truncated sequence yields U+FFFD and consumes exactly one
// byte, so a tokenizer can always make progress.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace bonk {

inline constexpr char32_t kReplacementCharacter = 0xFFFD;
inline constexpr char32_t kMaxCodePoint = 0x10FFFF;

struct DecodedCodePoint {
  char32_t code_point = 0;
  std::size_t length = 0;  // bytes consumed, always >= 1 for non-empty input
  bool valid = true;
};

/// Decode the sequence starting at `offset`. Precondition: offset < input.size().
[[nodiscard]] DecodedCodePoint decode_utf8(std::string_view input, std::size_t offset = 0) noexcept;

/// Append `code_point` as UTF-8. Surrogates and values > U+10FFFF are replaced by U+FFFD.
void append_utf8(std::string& out, char32_t code_point);

/// True when the whole buffer is well-formed UTF-8 (no U+FFFD substitution was needed).
[[nodiscard]] bool is_valid_utf8(std::string_view input) noexcept;

/// Number of code points (invalid bytes count as one U+FFFD each).
[[nodiscard]] std::size_t utf8_length(std::string_view input) noexcept;

[[nodiscard]] std::u32string decode_utf8_all(std::string_view input);
[[nodiscard]] std::string encode_utf8(std::u32string_view input);

// --- ASCII-only helpers (never locale dependent) -------------------------
[[nodiscard]] constexpr bool is_ascii(char32_t c) noexcept { return c < 0x80; }
[[nodiscard]] constexpr bool is_ascii_digit(char32_t c) noexcept { return c >= U'0' && c <= U'9'; }
[[nodiscard]] constexpr bool is_ascii_alpha(char32_t c) noexcept {
  return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}
[[nodiscard]] constexpr bool is_ascii_alnum(char32_t c) noexcept { return is_ascii_alpha(c) || is_ascii_digit(c); }
[[nodiscard]] constexpr char32_t to_ascii_lower(char32_t c) noexcept {
  return (c >= U'A' && c <= U'Z') ? c + 0x20 : c;
}
[[nodiscard]] constexpr char32_t to_ascii_upper(char32_t c) noexcept {
  return (c >= U'a' && c <= U'z') ? c - 0x20 : c;
}
/// WHATWG "ASCII whitespace": TAB, LF, FF, CR, SPACE.
[[nodiscard]] constexpr bool is_ascii_whitespace(char32_t c) noexcept {
  return c == U'\t' || c == U'\n' || c == U'\f' || c == U'\r' || c == U' ';
}

}  // namespace bonk
