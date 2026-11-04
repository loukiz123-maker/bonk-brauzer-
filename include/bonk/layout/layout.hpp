// Bonk Brauzer — layout.
//
// Stage-2 scope, fully implemented: block formatting context (with vertical
// margin collapsing), inline formatting context (line boxes, word wrapping,
// text-align), box model with em/rem/%/auto resolution, list markers, and
// inline-block images as atomic inlines.
//
// Not implemented yet (documented, stage 5): flexbox, grid, floats, absolute
// positioning, tables, and bidirectional text. Elements with those properties
// fall back to block/inline behaviour instead of disappearing.
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

  std::vector<Box> children;

  [[nodiscard]] float content_width() const noexcept { return content_box.width; }
  [[nodiscard]] float content_height() const noexcept { return content_box.height; }
};

struct LayoutViewport {
  float width = 1024.0f;
  float height = 768.0f;
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
/// by the shell in stage 3 and by `--hit-test` in the CLI).
[[nodiscard]] const Box* hit_test(const Box& root, float x, float y);

}  // namespace bonk::layout
