#include "test_framework.hpp"

#include "bonk/core/strings.hpp"

using namespace bonk;

BONK_TEST(strings, trim_variants) {
  CHECK_EQ(std::string(trim("  \t\r\n hello \n")), "hello");
  CHECK_EQ(std::string(trim_left("   x  ")), "x  ");
  CHECK_EQ(std::string(trim_right("   x  ")), "   x");
  CHECK_EQ(std::string(trim("   ")), "");
  CHECK_EQ(std::string(trim("")), "");
}

BONK_TEST(strings, split_full_semantics) {
  const auto parts = split("a,b,c", ',');
  REQUIRE(parts.size() == 3);
  CHECK_EQ(std::string(parts[0]), "a");
  CHECK_EQ(std::string(parts[2]), "c");

  const auto empty_fields = split("a,,b,", ',');
  REQUIRE(empty_fields.size() == 4);
  CHECK(empty_fields[1].empty());
  CHECK(empty_fields[3].empty());

  CHECK(split("", ',').empty());

  const auto no_delimiter = split("solo", ',');
  REQUIRE(no_delimiter.size() == 1);
  CHECK_EQ(std::string(no_delimiter[0]), "solo");
}

BONK_TEST(strings, join) {
  const std::vector<std::string_view> parts = {"a", "b", "c"};
  CHECK_EQ(join(parts, ", "), "a, b, c");
  CHECK_EQ(join({}, ", "), "");
}

BONK_TEST(strings, ascii_case_helpers) {
  CHECK(starts_with_ci("Content-Type", "content"));
  CHECK(!starts_with_ci("Content", "contents"));
  CHECK(equals_ci("DIV", "div"));
  CHECK(!equals_ci("div ", "div"));
  CHECK_EQ(to_ascii_lower("HeLLo-Я"), "hello-Я");  // non-ASCII untouched
  CHECK_EQ(to_ascii_upper("HeLLo"), "HELLO");
}

BONK_TEST(strings, replace_all) {
  CHECK_EQ(replace_all("a-b-c", "-", "::"), "a::b::c");
  CHECK_EQ(replace_all("aaa", "aa", "b"), "ba");  // non-overlapping, left to right
  CHECK_EQ(replace_all("abc", "", "x"), "abc");   // empty needle is a no-op
}

BONK_TEST(strings, padding) {
  CHECK_EQ(pad_right("ab", 5), "ab   ");
  CHECK_EQ(pad_right("abcdef", 3), "abcdef");
  CHECK_EQ(pad_left("ab", 4), "  ab");
}

BONK_TEST(strings, parse_u64) {
  unsigned long long value = 0;
  CHECK(parse_u64("0", value));
  CHECK_EQ(value, 0ull);
  CHECK(parse_u64("18446744073709551615", value));
  CHECK_EQ(value, 18446744073709551615ull);
  CHECK(!parse_u64("18446744073709551616", value));  // overflow
  CHECK(!parse_u64("", value));
  CHECK(!parse_u64("-1", value));
  CHECK(!parse_u64("1 ", value));
  CHECK(!parse_u64("+1", value));
}
