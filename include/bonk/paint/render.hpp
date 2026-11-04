// Bonk Brauzer — page renderer: layout boxes -> pixels.
//
// Paint order follows CSS 2.1 Appendix E for the subsets we support: background
// and borders of a block, then its children in document order (text runs, images,
// nested blocks). Stacking contexts, z-index and opacity groups are stage 6 work.
#pragma once

#include <string_view>

#include "bonk/css/css.hpp"
#include "bonk/layout/layout.hpp"
#include "bonk/paint/canvas.hpp"

namespace bonk::paint {

struct RenderOptions {
  css::Color page_background{255, 255, 255, 255};
  /// Draws 1px outlines around every block box (debugging aid: `--debug-boxes`).
  bool debug_boxes = false;
};

void render_layout(Canvas& canvas, const layout::Box& root, const text::Font& font, const RenderOptions& options);

}  // namespace bonk::paint
