// Bonk Brauzer — layout.
//
// Implemented: block formatting context (with vertical margin collapsing), inline
// formatting context (line boxes, word wrapping, text-align), box model with
// em/rem/%/auto resolution, list markers, inline-block images and canvases as
// atomic inlines, (stage 5) flexbox + grid containers and (stage 20) positioning:
// `position: relative | absolute | fixed` with `top/right/bottom/left`, containing
// block resolution and `z-index` paint order.
//
// Flexbox: row/column and their reverse, nowrap/wrap, justify-content, align-items,
// gap, flex-grow/shrink/basis. Grid: explicit `grid-template-columns/rows` with
// px/%/fr/auto and `repeat()`, auto-placement row by row, row/column gaps.
// Not implemented yet: floats, `position: sticky`, overflow clipping, tables in the
// HTML sense (only the `display: table*` boxes exist), bidi text, grid areas /
// explicit placement (grid-column/grid-row).
#pragma once

#include <string>
#include <vector>

#include "bonk/css/css.hpp"
#include "bonk/dom/dom.hpp"
#include "bonk/image/image.hpp"
#include "bonk/text/font.hpp"

namespace bonk::layout {

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  [[nodiscard]] bool contains(float px, float py) const noexcept {
    return px >= x && py >= y && px < x + width && py < y + height;
  }
  [[nodiscard]] float right() const noexcept { return x + width; }
  [[nodiscard]] float bottom() const noexcept { return y + height; }
};

enum class BoxKind {
  Block,      // block-level box (also list-item, flex fallback, table fallback)
  Text,       // one laid-out run of text on a line box
  Image,      // atomic inline (currently <img>, rendered as a placeholder)
  InlineBox,  // reserved for stage 5 (inline-block with children)
};

struct Box {
  BoxKind kind = BoxKind::Block;
  dom::NodeId node;
  const css::ComputedStyle* style = nullptr;

  Rect border_box;   // outer box, includes border and padding
  Rect content_box;  // inner box

  float margin[4] = {0, 0, 0, 0};  // top, right, bottom, left (resolved px)
  float padding[4] = {0, 0, 0, 0};
  float border[4] = {0, 0, 0, 0};

  float baseline = 0.0f;     // Text boxes: distance from border_box.y to the baseline
  float font_size = 16.0f;   // Text boxes: effective font size
  std::string text;          // Text boxes: the run itself
  bool has_marker = false;   // list-item bullet ("•")
  float marker_x = 0.0f;     // bullet position (left of the content box)
  const image::Image* image = nullptr;  // Image boxes: decoded bitmap, may be null

  /// Out-of-flow (`absolute`/`fixed`) box: it is not part of the normal flow, so its
  /// siblings ignore it. Painted with `positioned`, never with `children`.
  bool out_of_flow = false;
  /// Anchored to the viewport: painting ignores the scroll offset.
  bool fixed = false;
  int z_index = 0;  // effective z-index (0 unless `position` is not static and it is set)

  std::vector<Box> children;      // in-flow children, in document/paint order
  std::vector<Box> positioned;    // out-of-flow children; painted above `children`

  [[nodiscard]] float content_width() const noexcept { return content_box.width; }
  [[nodiscard]] float content_height() const noexcept { return content_box.height; }
};

/// A <canvas> bitmap provided by the bindings layer: layout treats the element as a
/// replaced inline (like <img>) and paint blits the pixels.
struct CanvasBitmap {
  dom::NodeId node;
  const image::Image* image = nullptr;
};

struct LayoutViewport {
  float width = 1024.0f;
  float height = 768.0f;
  /// Canvas bitmaps for the page being laid out (may be null).
  const std::vector<CanvasBitmap>* canvases = nullptr;
};

struct LayoutResult {
  Box root;                  // the <html> element box (or the body if no <html>)
  float document_height = 0.0f;
  float content_width = 0.0f;
};

/// Lays out the document. `font` is used for text metrics; `styles` must already
/// be computed for the same document and viewport width.
/// `images` may be null: then <img> boxes fall back to their width/height attributes
/// or a default placeholder size.
[[nodiscard]] LayoutResult layout_document(const dom::Document& document, const css::StyleMap& styles,
                                           const text::Font& font, const LayoutViewport& viewport,
                                           const image::ImageStore* images = nullptr);

/// Finds the deepest box containing a point (hit testing for links/clicks, used
/// by the shell in stage 3 and by `--hit-test` in the CLI). `x`/`y` are document
/// coordinates: pass the current `scroll_y` so that `position: fixed` boxes — which
/// are laid out in viewport space — are tested against the right point.
[[nodiscard]] const Box* hit_test(const Box& root, float x, float y, float scroll_y = 0.0f);

}  // namespace bonk::layout
