#include "bonk/core/utf8.hpp"

#include <cstdint>

namespace bonk {
namespace {

// One continuation byte? (also enforces the range a given lead byte allows)
inline bool is_continuation(unsigned char byte) noexcept { return (byte & 0xC0u) == 0x80u; }
inline bool in_range(unsigned char byte, unsigned char low, unsigned char high) noexcept {
  return byte >= low && byte <= high;
}

DecodedCodePoint invalid(std::size_t length = 1) noexcept {
  return DecodedCodePoint{kReplacementCharacter, length, false};
}

}  // namespace

DecodedCodePoint decode_utf8(std::string_view input, std::size_t offset) noexcept {
  if (offset >= input.size()) return invalid(0);

  const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
  const std::size_t available = input.size() - offset;
  const unsigned char b0 = bytes[offset];

  if (b0 < 0x80) return DecodedCodePoint{b0, 1, true};

  // Two-byte sequences: U+0080..U+07FF. 0xC0/0xC1 can only encode overlongs.
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (available < 2 || !is_continuation(bytes[offset + 1])) return invalid();
    const char32_t cp = (static_cast<char32_t>(b0 & 0x1Fu) << 6) | (bytes[offset + 1] & 0x3Fu);
    return DecodedCodePoint{cp, 2, true};
  }

  // Three-byte sequences: U+0800..U+FFFF, minus surrogates (0xED A0..BF).
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    if (available < 3) return invalid();
    const unsigned char b1 = bytes[offset + 1];
    const unsigned char b2 = bytes[offset + 2];
    unsigned char low = 0x80u, high = 0xBFu;
    if (b0 == 0xE0) low = 0xA0u;  // overlong guard
    if (b0 == 0xED) high = 0x9Fu;  // surrogate guard
    if (!in_range(b1, low, high) || !is_continuation(b2)) return invalid();
    const char32_t cp = (static_cast<char32_t>(b0 & 0x0Fu) << 12) | (static_cast<char32_t>(b1 & 0x3Fu) << 6) |
                        (b2 & 0x3Fu);
    return DecodedCodePoint{cp, 3, true};
  }

  // Four-byte sequences: U+10000..U+10FFFF.
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    if (available < 4) return invalid();
    const unsigned char b1 = bytes[offset + 1];
    const unsigned char b2 = bytes[offset + 2];
    const unsigned char b3 = bytes[offset + 3];
    unsigned char low = 0x80u, high = 0xBFu;
    if (b0 == 0xF0) low = 0x90u;  // overlong guard
    if (b0 == 0xF4) high = 0x8Fu;  // > U+10FFFF guard
    if (!in_range(b1, low, high) || !is_continuation(b2) || !is_continuation(b3)) return invalid();
    const char32_t cp = (static_cast<char32_t>(b0 & 0x07u) << 18) | (static_cast<char32_t>(b1 & 0x3Fu) << 12) |
                        (static_cast<char32_t>(b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
    return DecodedCodePoint{cp, 4, true};
  }

  // 0x80..0xBF (stray continuation), 0xC0/0xC1, 0xF5..0xFF.
  return invalid();
}

void append_utf8(std::string& out, char32_t code_point) {
  if (code_point > kMaxCodePoint || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    code_point = kReplacementCharacter;
  }

  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
}

bool is_valid_utf8(std::string_view input) noexcept {
  std::size_t offset = 0;
  while (offset < input.size()) {
    const DecodedCodePoint decoded = decode_utf8(input, offset);
    if (!decoded.valid) return false;
    offset += decoded.length;
  }
  return true;
}

std::size_t utf8_length(std::string_view input) noexcept {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < input.size()) {
    offset += decode_utf8(input, offset).length;
    ++count;
  }
  return count;
}

std::u32string decode_utf8_all(std::string_view input) {
  std::u32string out;
  out.reserve(input.size());
  std::size_t offset = 0;
  while (offset < input.size()) {
    const DecodedCodePoint decoded = decode_utf8(input, offset);
    out.push_back(decoded.code_point);
    offset += decoded.length;
  }
  return out;
}

std::string encode_utf8(std::u32string_view input) {
  std::string out;
  out.reserve(input.size());
  for (const char32_t code_point : input) append_utf8(out, code_point);
  return out;
}

}  // namespace bonk
