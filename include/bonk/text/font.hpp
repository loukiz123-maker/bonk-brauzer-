// Bonk Brauzer — font loading and glyph rasterization.
//
// A TrueType/OpenType (glyf-flavoured) parser written from scratch:
//   head/hhea/maxp/hmtx/loca/glyf/cmap(4,12), simple and composite contours,
//   quadratic curve flattening, non-zero winding scanline fill with 4x4
//   supersampling. No hinting (documented limitation: hinting is a stage-6
//   optimisation, not a correctness issue at the sizes we render).
//
// If no font file is available the engine falls back to a built-in "tofu"
// renderer: every glyph is a visible box with the character's code point, so a
// page still renders legibly enough to debug layout. `bonk render --font PATH`
// or BONK_FONT select a real font; system DejaVu is auto-detected on Linux.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk::text {

struct GlyphBitmap {
  int width = 0;
  int height = 0;
  int left = 0;  // x offset from the pen position, px
  int top = 0;   // y offset above the baseline, px (positive = up)
  std::vector<std::uint8_t> alpha;  // width*height coverage, 0..255

  [[nodiscard]] std::uint8_t at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    return alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
  }
};

class Font {
 public:
  /// Empty font = built-in tofu fallback (never fails).
  Font();
  ~Font();
  Font(const Font&);
  Font& operator=(const Font&);
  Font(Font&&) noexcept;
  Font& operator=(Font&&) noexcept;

  [[nodiscard]] static Result<Font> load_file(std::string_view path);
  [[nodiscard]] static Result<Font> load_memory(std::string_view bytes, std::string name = {});
  /// Tries $BONK_FONT, then a list of common system paths; empty font if none.
  [[nodiscard]] static Font system_default();

  [[nodiscard]] bool is_fallback() const noexcept;
  [[nodiscard]] std::string_view name() const noexcept;

  [[nodiscard]] float ascent(float pixel_size) const;
  [[nodiscard]] float descent(float pixel_size) const;  // positive number below the baseline
  [[nodiscard]] float line_height(float pixel_size) const;
  [[nodiscard]] float advance(char32_t code_point, float pixel_size) const;
  [[nodiscard]] float space_width(float pixel_size) const;
  [[nodiscard]] float text_width(std::string_view utf8, float pixel_size) const;

  /// Cached coverage mask; `pixel_size` is rounded to whole pixels.
  [[nodiscard]] const GlyphBitmap& glyph(char32_t code_point, float pixel_size) const;

  /// True when the font has a real glyph for this code point.
  [[nodiscard]] bool has_glyph(char32_t code_point) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace bonk::text
