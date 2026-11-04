// Bonk Brauzer — software (CPU) raster canvas.
//
// Pixels are RGBA8 premultiplied-by-nothing (straight alpha), row-major. All
// drawing operations clip to the canvas rectangle themselves: a page cannot
// crash the rasterizer by drawing outside the viewport.
//
// PNG output is written by our own encoder: zlib stream with stored
// (uncompressed) deflate blocks. That is valid PNG, costs no entropy coding, and
// keeps the "no external dependencies" rule while the real deflate encoder is a
// later optimisation.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/css/css.hpp"
#include "bonk/layout/layout.hpp"
#include "bonk/text/font.hpp"

namespace bonk::paint {

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

class Canvas {
 public:
  Canvas(int width, int height);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

  void clear(css::Color color);
  void fill_rect(Rect rect, css::Color color);
  void stroke_rect(Rect rect, css::Color color, float thickness);
  void blend_pixel(int x, int y, css::Color color);
  /// Draws one glyph with its baseline at `baseline_y`.
  void draw_glyph(const text::Font& font, char32_t code_point, float x, float baseline_y, float font_size,
                  css::Color color);
  /// Draws a UTF-8 string, returning the pen x after the last glyph.
  float draw_text(const text::Font& font, std::string_view text, float x, float baseline_y, float font_size,
                  css::Color color);
  [[nodiscard]] css::Color pixel_at(int x, int y) const;

  /// PNG bytes from our own encoder (used by `write_png` and by the isolated
  /// tab process, which ships the frame over a pipe instead of a file).
  [[nodiscard]] std::string encode_png() const;
  [[nodiscard]] bool write_png(std::string_view path) const;
  [[nodiscard]] bool write_ppm(std::string_view path) const;

 private:
  int width_ = 0;
  int height_ = 0;
  std::vector<std::uint8_t> pixels_;
};

}  // namespace bonk::paint
