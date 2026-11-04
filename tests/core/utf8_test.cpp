#include <cstring>

#include "test_framework.hpp"

#include "bonk/core/utf8.hpp"

using namespace bonk;

BONK_TEST(utf8, ascii) {
  const DecodedCodePoint decoded = decode_utf8("A");
  CHECK(decoded.valid);
  CHECK_EQ(decoded.code_point, U'A');
  CHECK_EQ(decoded.length, std::size_t{1});
}

BONK_TEST(utf8, boundary_code_points) {
  struct Sample {
    char32_t code_point;
    const char* bytes;
  };
  const Sample samples[] = {
      {0x7F, "\x7F"},
      {0x80, "\xC2\x80"},
      {0x7FF, "\xDF\xBF"},
      {0x800, "\xE0\xA0\x80"},
      {0xFFFD, "\xEF\xBF\xBD"},
      {0xFFFF, "\xEF\xBF\xBF"},
      {0x10000, "\xF0\x90\x80\x80"},
      {0x10FFFF, "\xF4\x8F\xBF\xBF"},
  };
  for (const Sample& sample : samples) {
    std::string encoded;
    append_utf8(encoded, sample.code_point);
    CHECK_EQ(encoded, std::string(sample.bytes));
    const DecodedCodePoint decoded = decode_utf8(encoded);
    CHECK(decoded.valid);
    CHECK_EQ(decoded.code_point, sample.code_point);
    CHECK_EQ(decoded.length, encoded.size());
  }
}

BONK_TEST(utf8, rejects_overlong_and_surrogates) {
  const char* invalid[] = {
      "\xC0\x80",          // overlong NUL
      "\xC1\xBF",          // overlong
      "\xE0\x80\x80",      // overlong
      "\xED\xA0\x80",      // U+D800 surrogate
      "\xED\xBF\xBF",      // U+DFFF surrogate
      "\xF0\x80\x80\x80",  // overlong
      "\xF4\x90\x80\x80",  // > U+10FFFF
      "\xF5\x80\x80\x80",  // invalid lead byte
      "\x80",              // stray continuation
      "\xFE",
  };
  for (const char* bytes : invalid) {
    const std::string input(bytes, std::strlen(bytes));
    CHECK(!is_valid_utf8(input));
    const DecodedCodePoint decoded = decode_utf8(input);
    CHECK(!decoded.valid);
    CHECK_EQ(decoded.code_point, kReplacementCharacter);
    CHECK_EQ(decoded.length, std::size_t{1});
  }
}

BONK_TEST(utf8, truncated_sequence_consumes_one_byte) {
  const std::string truncated("\xE2\x82", 2);  // first two bytes of U+20AC EURO
  const DecodedCodePoint decoded = decode_utf8(truncated, 0);
  CHECK(!decoded.valid);
  CHECK_EQ(decoded.length, std::size_t{1});
  // The tokenizer can always make progress.
  CHECK_EQ(utf8_length(truncated), std::size_t{2});
}

BONK_TEST(utf8, cyrillic_roundtrip) {
  const std::string text = "Привет, мир! Ёё";
  const std::u32string code_points = decode_utf8_all(text);
  CHECK_EQ(code_points.size(), utf8_length(text));
  CHECK_EQ(encode_utf8(code_points), text);
  CHECK(is_valid_utf8(text));
}

BONK_TEST(utf8, append_replaces_surrogates) {
  std::string out;
  append_utf8(out, 0xD800);
  CHECK_EQ(out, std::string("\xEF\xBF\xBD"));
  out.clear();
  append_utf8(out, 0x110000);
  CHECK_EQ(out, std::string("\xEF\xBF\xBD"));
}

BONK_TEST(utf8, offset_decoding) {
  const std::string text = "aяb";
  CHECK_EQ(decode_utf8(text, 1).code_point, char32_t{0x44F});
  CHECK_EQ(decode_utf8(text, 3).code_point, U'b');
  CHECK_EQ(decode_utf8(text, 3).length, std::size_t{1});
}
