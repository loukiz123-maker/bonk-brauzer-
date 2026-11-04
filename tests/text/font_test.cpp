#include <cstdio>
#include <string>

#include "test_framework.hpp"

#include "bonk/text/font.hpp"

using namespace bonk;

namespace {

bool file_exists(const char* path) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) return false;
  std::fclose(file);
  return true;
}

const char* kDejaVu = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

}  // namespace

BONK_TEST(font, fallback_metrics_are_usable) {
  const text::Font font;  // built-in tofu fallback
  CHECK(font.is_fallback());
  CHECK(font.ascent(16.0f) > 0.0f);
  CHECK(font.descent(16.0f) > 0.0f);
  CHECK(font.line_height(16.0f) > font.ascent(16.0f));
  CHECK(font.advance(U'A', 16.0f) > 0.0f);
  CHECK(font.space_width(16.0f) > 0.0f);
  CHECK_EQ(font.text_width("abc", 16.0f), font.advance(U'a', 16.0f) * 3.0f);

  const text::GlyphBitmap& glyph = font.glyph(U'A', 16.0f);
  CHECK(glyph.width > 0);
  CHECK(glyph.height > 0);
  CHECK(!glyph.alpha.empty());
}

BONK_TEST(font, fallback_space_has_no_ink) {
  const text::Font font;
  const text::GlyphBitmap& space = font.glyph(U' ', 16.0f);
  bool has_ink = false;
  for (const std::uint8_t value : space.alpha) {
    if (value > 0) has_ink = true;
  }
  CHECK(!has_ink);
}

BONK_TEST(font, bad_input_is_rejected) {
  const Result<text::Font> tiny = text::Font::load_memory("abc");
  REQUIRE(tiny.is_error());
  CHECK_EQ(tiny.error().code, std::string("font.truncated"));

  const Result<text::Font> garbage = text::Font::load_memory(std::string(64, 'x'));
  REQUIRE(garbage.is_error());
  CHECK_EQ(garbage.error().code, std::string("font.unknown_format"));

  const Result<text::Font> missing = text::Font::load_file("/nonexistent/font.ttf");
  REQUIRE(missing.is_error());
  CHECK_EQ(missing.error().code, std::string("font.open_failed"));
}

BONK_TEST(font, truetype_parsing_and_rasterization) {
  if (!file_exists(kDejaVu)) {
    // No system font on this machine: the fallback is what users would get.
    CHECK(text::Font::system_default().is_fallback());
    return;
  }

  const Result<text::Font> loaded = text::Font::load_file(kDejaVu);
  REQUIRE(loaded.is_ok());
  const text::Font& font = loaded.value();
  CHECK(!font.is_fallback());
  CHECK(font.ascent(20.0f) > 10.0f);
  CHECK(font.line_height(20.0f) > 15.0f);

  CHECK(font.has_glyph(U'A'));
  CHECK(font.has_glyph(U'я'));
  CHECK(font.advance(U'i', 20.0f) > 0.0f);
  CHECK(font.advance(U'W', 20.0f) > font.advance(U'i', 20.0f));  // proportional font

  const text::GlyphBitmap& glyph = font.glyph(U'A', 20.0f);
  CHECK(glyph.width > 4);
  CHECK(glyph.height > 8);
  int inked = 0;
  for (const std::uint8_t value : glyph.alpha) {
    if (value > 0) ++inked;
  }
  CHECK(inked > 10);

  // Caching must return the exact same object for the same request.
  CHECK(&font.glyph(U'A', 20.0f) == &glyph);

  // Missing glyphs still produce a visible .notdef box.
  const text::GlyphBitmap& tofu = font.glyph(0x4F60, 20.0f);  // 你 — absent from DejaVu Sans
  CHECK(tofu.width > 0);
  int tofu_ink = 0;
  for (const std::uint8_t value : tofu.alpha) {
    if (value > 0) ++tofu_ink;
  }
  CHECK(tofu_ink > 0);

  // Space is empty ink but has width.
  const text::GlyphBitmap& space = font.glyph(U' ', 20.0f);
  CHECK(space.alpha.empty() || space.width <= 1);
  CHECK(font.space_width(20.0f) > 0.0f);

  // Scaled text keeps proportional metrics.
  CHECK(font.text_width("Hello", 40.0f) > font.text_width("Hello", 20.0f) * 1.5f);
}

BONK_TEST(font, utf8_text_width_handles_multibyte) {
  const text::Font font;
  const float cyrillic = font.text_width("привет", 16.0f);
  CHECK(cyrillic > 0.0f);
  CHECK_EQ(font.text_width("", 16.0f), 0.0f);
}
