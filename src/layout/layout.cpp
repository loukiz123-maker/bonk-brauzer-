#include "bonk/layout/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"

namespace bonk::layout {
namespace {

constexpr float kListIndent = 24.0f;
constexpr float kMarkerWidth = 12.0f;

float resolve(const css::Length& length, float font_size, float containing_width) {
  switch (length.unit) {
    case css::Length::Unit::Auto:
      return 0.0f;
    case css::Length::Unit::Percent:
      return length.value * containing_width / 100.0f;
    case css::Length::Unit::Em:
      return length.value * font_size;
    case css::Length::Unit::Rem:
      return length.value * 16.0f;
    default:
      return css::absolute_length_to_px(length);
  }
}

struct InlineItem {
  enum class Kind { Text, Atomic } kind = Kind::Text;
  dom::NodeId node;
  const css::ComputedStyle* style = nullptr;
  std::string text;
  float width = 0.0f;
  float height = 0.0f;
  const image::Image* image = nullptr;  // Atomic items: picture to blit (may be null)
  bool inline_block = false;            // Atomic item with its own content (input, button, …)
  bool is_space = false;  // collapses/breaks lines
  bool is_float = false;  // marker: place a float here, no width of its own
};

/// One float currently visible to line boxes of the current block formatting
/// context. `rect` is the border box in absolute coordinates.
struct FloatRect {
  Rect rect;
  css::FloatSide side = css::FloatSide::None;
};

/// Where an out-of-flow box would have been placed had it stayed in the flow. Only
/// recorded for block-level children; elsewhere the containing block origin is used.
struct StaticPosition {
  dom::NodeId node;
  float x = 0.0f;
  float y = 0.0f;
};

class LayoutEngine {
 public:
  LayoutEngine(const dom::Document& document, const css::StyleMap& styles, const text::Font& font,
               const LayoutViewport& viewport, const image::ImageStore* images)
      : document_(document), styles_(styles), font_(font), viewport_(viewport), images_(images) {}

  LayoutResult run() {
    LayoutResult result;
    const dom::NodeId root_node = document_.document_element();
    if (!root_node.valid()) {
      result.document_height = 0.0f;
      return result;
    }

    const css::ComputedStyle& root_style = styles_.get(document_, root_node);
    Box root;
    root.kind = BoxKind::Block;
    root.node = root_node;
    root.style = &root_style;
    root.border_box = Rect{0.0f, 0.0f, viewport_.width, 0.0f};
    root.content_box = root.border_box;

    float height = layout_block_children(root, 0.0f, 0.0f, viewport_.width);
    root.content_box.height = height;
    root.border_box.height = height;

    // Positioning is a second pass: insets are resolved against a containing block,
    // which only has final coordinates once the whole normal flow is laid out.
    apply_relative_offsets(root);
    place_positioned(root, true);
    height = std::max(height, floats_bottom());
    height = std::max(height, positioned_bottom(root));
    root.content_box.height = height;
    root.border_box.height = height;

    result.root = std::move(root);
    result.document_height = height;
    result.content_width = viewport_.width;
    return result;
  }

 private:
  // --- box metrics -------------------------------------------------------
  void compute_edges(Box& box, const css::ComputedStyle& style, float containing_width) {
    for (int i = 0; i < 4; ++i) {
      box.margin[i] = resolve(style.margin[i], style.font_size, containing_width);
      box.padding[i] = resolve(style.padding[i], style.font_size, containing_width);
      box.border[i] = style.border_width[i];
    }
  }

  [[nodiscard]] bool is_block_level(const css::ComputedStyle& style) const {
    switch (style.display) {
      case css::Display::Block:
      case css::Display::ListItem:
      case css::Display::Flex:
      case css::Display::Grid:
      case css::Display::Table:
      case css::Display::TableRow:
      case css::Display::TableCell:
        return true;
      default:
        return false;
    }
  }

  /// Border-box size of an inline-block: content width from max-content (or the
  /// declared width), clamped by min/max width, height from a probe layout.
  void measure_inline_block(InlineItem& item, const css::ComputedStyle& style, dom::NodeId node) {
    const float px = viewport_.width;
    const float pad_l = resolve(style.padding[3], style.font_size, px);
    const float pad_r = resolve(style.padding[1], style.font_size, px);
    const float pad_t = resolve(style.padding[0], style.font_size, px);
    const float pad_b = resolve(style.padding[2], style.font_size, px);
    const float bw_l = style.border_width[3];
    const float bw_r = style.border_width[1];
    const float bw_t = style.border_width[0];
    const float bw_b = style.border_width[2];
    const float horizontal = pad_l + pad_r + bw_l + bw_r;
    const float vertical = pad_t + pad_b + bw_t + bw_b;

    float content = style.width.is_auto() ? intrinsic_width(node) : resolve(style.width, style.font_size, px);
    if (style.min_width > 0.0f) content = std::max(content, style.min_width);
    if (style.max_width > 0.0f) content = std::min(content, style.max_width);
    content = std::max(0.0f, content);

    Box probe;
    init_child_box(probe, node, style, 0.0f, 0.0f, content + horizontal);
    float height = layout_content(probe, probe.content_box.width) + vertical;
    if (!style.height.is_auto()) height = resolve(style.height, style.font_size, px) + vertical;
    if (style.min_height > 0.0f) height = std::max(height, style.min_height + vertical);

    item.width = content + horizontal;
    item.height = height;
  }

  // --- inline content ----------------------------------------------------
  void collect_inline_items(dom::NodeId parent, std::vector<InlineItem>& items) {
    collect_inline_items_from(document_.children(parent), items);
  }

  /// Same as `collect_inline_items`, but for an explicit run of siblings — used
  /// for the anonymous blocks that hold inline runs between block children.
  void collect_inline_items_from(const std::vector<dom::NodeId>& children, std::vector<InlineItem>& items) {
    for (const dom::NodeId child : children) {
      const dom::NodeType type = document_.type(child);
      if (type == dom::NodeType::Text) {
        const css::ComputedStyle& style = styles_.get(document_, child);
        append_text_items(child, document_.text_data(child), style, items);
        continue;
      }
      if (type != dom::NodeType::Element) continue;

      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;
      if (style.is_out_of_flow()) continue;
      if (style.is_floating()) {
        // A float inside an inline run (`<p><img style="float:left">text</p>`) is
        // placed by the run itself, at the line where it appears; the lines that
        // follow wrap around it.
        InlineItem item;
        item.node = child;
        item.style = &style;
        item.is_float = true;
        items.push_back(std::move(item));
        continue;
      }

      if (document_.tag_name(child) == "br") {
        // <br> is a hard line break, exactly like "\n" inside <pre>.
        InlineItem item;
        item.kind = InlineItem::Kind::Text;
        item.node = child;
        item.style = &style;
        item.text = "\n";
        items.push_back(std::move(item));
        continue;
      }

      if (document_.tag_name(child) == "br") {
        // <br>: a hard line break, shared with "\n" inside <pre>.
        InlineItem item;
        item.kind = InlineItem::Kind::Text;
        item.node = child;
        item.style = &style;
        item.text = "\n";
        items.push_back(std::move(item));
        continue;
      }

      if (document_.tag_name(child) == "img" || document_.tag_name(child) == "canvas") {
        InlineItem item;
        item.kind = InlineItem::Kind::Atomic;
        item.node = child;
        item.style = &style;

        // Size priority: HTML width/height attributes, then the bitmap's
        // intrinsic size, then a placeholder that keeps the line height sane.
        unsigned long long width_value = 0;
        unsigned long long height_value = 0;
        if (parse_u64(document_.attribute(child, "width"), width_value)) {
          item.width = static_cast<float>(width_value);
        }
        if (parse_u64(document_.attribute(child, "height"), height_value)) {
          item.height = static_cast<float>(height_value);
        }
        const bool is_canvas = document_.tag_name(child) == "canvas";
        const image::Image* bitmap =
            is_canvas ? replaced_image(child)
                      : (images_ == nullptr ? nullptr : images_->find(document_.attribute(child, "src")));
        if (bitmap != nullptr) {
          if (item.width <= 0.0f) item.width = static_cast<float>(bitmap->width);
          if (item.height <= 0.0f) item.height = static_cast<float>(bitmap->height);
        }
        if (is_canvas) {
          if (item.width <= 0.0f) item.width = 300.0f;   // HTML default canvas size
          if (item.height <= 0.0f) item.height = 150.0f;
        } else {
          if (item.width <= 0.0f) item.width = std::min(120.0f, viewport_.width / 2.0f);
          if (item.height <= 0.0f) item.height = item.width * 0.6f;
        }
        item.image = bitmap;
        items.push_back(std::move(item));
        continue;
      }

      if (style.display == css::Display::InlineBlock) {
        // Inline-block: an atomic inline box that still has its own content.
        // Measure it here (like a float), lay the content out during placement.
        InlineItem item;
        item.kind = InlineItem::Kind::Atomic;
        item.node = child;
        item.style = &style;
        item.inline_block = true;
        measure_inline_block(item, style, child);
        items.push_back(std::move(item));
        continue;
      }

      if (is_block_level(style)) {
        // A block inside an inline context: flush inline items first, then lay
        // out as block. (Full CSS would split the inline context; we end it.)
        InlineItem item;
        item.kind = InlineItem::Kind::Atomic;
        item.node = child;
        item.style = &style;
        item.width = 0.0f;
        item.height = 0.0f;
        items.push_back(std::move(item));
        continue;
      }

      // Inline element: recurse, inheriting its style for the text it contains.
      collect_inline_items(child, items);
    }
  }

  void append_text_items(dom::NodeId node, std::string_view text, const css::ComputedStyle& style,
                         std::vector<InlineItem>& items) {
    if (style.visible == false) return;
    if (text.empty()) return;

    const bool pre = style.white_space_pre;
    std::string collapsed;
    bool last_was_space = items.empty() || items.back().is_space;
    for (const char c : text) {
      const bool is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
      if (!pre && is_space) {
        if (last_was_space) continue;
        collapsed.push_back(' ');
        last_was_space = true;
      } else if (pre && c == '\n') {
        InlineItem item;
        item.kind = InlineItem::Kind::Text;
        item.node = node;
        item.style = &style;
        item.text = "\n";
        item.width = 0.0f;
        item.is_space = false;
        items.push_back(std::move(item));
        collapsed.clear();
        last_was_space = true;
      } else {
        collapsed.push_back(c);
        last_was_space = false;
      }
    }
    if (collapsed.empty()) return;

    // Split into words and single spaces so that lines can break between words.
    std::size_t start = 0;
    while (start < collapsed.size()) {
      std::size_t end = collapsed.find(' ', start);
      if (end == std::string::npos) end = collapsed.size();
      if (end > start) {
        InlineItem word;
        word.kind = InlineItem::Kind::Text;
        word.node = node;
        word.style = &style;
        word.text = collapsed.substr(start, end - start);
        word.width = font_.text_width(word.text, style.font_size);
        word.height = style.effective_line_height();
        items.push_back(std::move(word));
      }
      if (end < collapsed.size()) {
        InlineItem space;
        space.kind = InlineItem::Kind::Text;
        space.node = node;
        space.style = &style;
        space.text = " ";
        space.width = font_.space_width(style.font_size);
        space.height = style.effective_line_height();
        space.is_space = true;
        items.push_back(std::move(space));
      }
      start = end + 1;
    }
  }

  /// Lays out one inline formatting context, appending Text/Image boxes to `box`.
  float layout_inline(Box& box, float content_x, float start_y, float content_width) {
    std::vector<InlineItem> items;
    collect_inline_items(box.node, items);
    return layout_inline_items(box, content_x, start_y, content_width, items);
  }

  /// Lays out one inline formatting context (a run of inline-level siblings,
  /// boxed into an anonymous block by the block layout) and appends its boxes.
  float layout_inline_items(Box& box, float content_x, float start_y, float content_width,
                            const std::vector<InlineItem>& items) {
    float y = start_y;
    std::size_t index = 0;

    // Floats shorten the line boxes they cross. A nominal line height is used to pick
    // the span: it is the tallest item of this run, which is what lines will be in the
    // overwhelming majority of cases.
    float nominal_height = 0.0f;
    for (const InlineItem& item : items) nominal_height = std::max(nominal_height, item.height);
    if (nominal_height <= 0.0f) nominal_height = box.style->effective_line_height();

    while (index < items.size()) {
      // Floats that start the line are placed before its span is computed, so the
      // very first line already wraps around them.
      while (index < items.size() && items[index].is_float) {
        const InlineItem& float_item = items[index++];
        box.children.push_back(place_float(float_item.node, *float_item.style, content_x, y, content_width));
      }
      if (index >= items.size()) break;

      // Gather one line.
      std::vector<const InlineItem*> line;
      float line_width = 0.0f;
      float line_height = 0.0f;
      float line_ascent = 0.0f;
      std::size_t last_break = items.size();  // index of the last breakable space

      float span_left = content_x;
      float span_right = content_x + content_width;
      for (int attempt = 0; attempt < 8; ++attempt) {
        span_at(content_x, content_width, y, nominal_height, span_left, span_right);
        if (span_right - span_left > 1.0f) break;
        // The floats cover this line entirely: move below the lowest of them.
        const float next = next_float_bottom(y);
        if (next <= y + 0.01f) break;
        y = next;
      }
      const float line_available = std::max(0.0f, span_right - span_left);

      while (index < items.size()) {
        const InlineItem& item = items[index];
        if (item.is_float) {
          // A float in the middle of a line: place it, keep this line, let the next one
          // wrap. (Full CSS would only restart the line if it no longer fits.)
          box.children.push_back(place_float(item.node, *item.style, content_x, y, content_width));
          ++index;
          continue;
        }
        if (item.kind == InlineItem::Kind::Text && item.text == "\n") {  // <pre> hard break
          ++index;
          break;
        }
        const bool fits = line_width + item.width <= line_available + 0.01f;
        if (!fits && !line.empty() && !(line.size() == 1 && line.back()->is_space)) break;
        line.push_back(&item);
        line_width += item.width;
        const float item_height = item.kind == InlineItem::Kind::Atomic ? item.height : item.height;
        const float item_ascent = item.kind == InlineItem::Kind::Atomic
                                      ? item.height
                                      : font_.ascent(item.style->font_size);
        line_height = std::max(line_height, item_height);
        line_ascent = std::max(line_ascent, item_ascent);
        if (item.kind == InlineItem::Kind::Text && item.is_space) last_break = line.size() - 1;
        ++index;
      }

      if (line.empty()) {
        if (index < items.size() && items[index].kind == InlineItem::Kind::Atomic) {
          // Oversized atomic element: place it on its own line.
          line.push_back(&items[index]);
          line_width = items[index].width;
          line_height = items[index].height;
          line_ascent = items[index].height;
          ++index;
        } else {
          ++index;
          continue;
        }
      }

      // Drop trailing spaces from the visible line and wrap at the last space if needed.
      if (line_width > line_available && last_break < line.size()) {
        index = index - (line.size() - last_break - 1);
        line.resize(last_break);
        line_width = 0.0f;
        line_height = 0.0f;
        line_ascent = 0.0f;
        for (const InlineItem* item : line) {
          line_width += item->width;
          line_height = std::max(line_height, item->height);
          line_ascent = std::max(line_ascent, item->kind == InlineItem::Kind::Atomic
                                                  ? item->height
                                                  : font_.ascent(item->style->font_size));
        }
      }
      while (!line.empty() && line.back()->is_space) {
        line_width -= line.back()->width;
        line.pop_back();
      }
      if (line_height <= 0.0f) line_height = box.style->effective_line_height();

      // Horizontal alignment inside the span the floats left free.
      float x = span_left;
      const float free_space = std::max(0.0f, line_available - line_width);
      switch (box.style->text_align) {
        case css::TextAlign::Center:
          x += free_space / 2.0f;
          break;
        case css::TextAlign::Right:
          x += free_space;
          break;
        default:
          break;
      }

      for (const InlineItem* item : line) {
        if (item->kind == InlineItem::Kind::Atomic && item->inline_block) {
          // Sit the inline-block on the line's baseline box (bottom aligned).
          const float block_y = y + std::max(0.0f, line_height - item->height);
          Box child;
          child.kind = BoxKind::InlineBox;
          child.node = item->node;
          child.style = item->style;
          init_child_box(child, item->node, *item->style, x, block_y, item->width);
          child.border_box = Rect{x, block_y, item->width, item->height};
          child.content_box = Rect{x + child.border[3] + child.padding[3],
                                   y + (block_y - y) + child.border[0] + child.padding[0],
                                   std::max(0.0f, item->width - (child.border[3] + child.padding[3] +
                                                                 child.border[1] + child.padding[1])),
                                   std::max(0.0f, item->height - (child.border[0] + child.padding[0] +
                                                                  child.border[2] + child.padding[2]))};
          child.font_size = item->style->font_size;
          {
            FloatGuard guard(floats_);  // an inline-block establishes a formatting context
            (void)layout_content(child, child.content_box.width);
          }
          box.children.push_back(std::move(child));
        } else if (item->kind == InlineItem::Kind::Atomic) {
          Box child;
          child.kind = document_.tag_name(item->node) == "img" || document_.tag_name(item->node) == "canvas"
                           ? BoxKind::Image
                           : BoxKind::InlineBox;
          child.node = item->node;
          child.style = item->style;
          child.border_box = Rect{x, y, item->width, item->height};
          child.content_box = child.border_box;
          child.font_size = item->style->font_size;
          child.image = item->image;  // <img> from the image store, <canvas> from the bindings
          box.children.push_back(std::move(child));
        } else {
          Box child;
          child.kind = BoxKind::Text;
          child.node = item->node;
          child.style = item->style;
          child.text = item->text;
          child.font_size = item->style->font_size;
          child.baseline = line_ascent;
          child.border_box = Rect{x, y, item->width, line_height};
          child.content_box = child.border_box;
          box.children.push_back(std::move(child));
        }
        x += item->width;
      }

      y += line_height;
    }

    return y - start_y;
  }


  // --- flexbox / grid (stage 5) ------------------------------------------
  [[nodiscard]] bool is_flex_or_grid(const css::ComputedStyle& style) const {
    return style.display == css::Display::Flex || style.display == css::Display::Grid;
  }

  /// Max-content width estimate of a subtree: inline children add up on one
  /// line, block children start a new line. Flex items with `width: auto` use
  /// this as their base size.
  [[nodiscard]] float intrinsic_width(dom::NodeId node) const {
    float best = 0.0f;
    float line = 0.0f;
    for (const dom::NodeId child : document_.children(node)) {
      const dom::NodeType type = document_.type(child);
      if (type == dom::NodeType::Text) {
        const css::ComputedStyle& style = styles_.get(document_, child);
        line += font_.text_width(document_.text_data(child), style.font_size);
        continue;
      }
      if (type != dom::NodeType::Element) continue;
      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;
      if (style.is_out_of_flow()) continue;
      if (style.is_floating()) continue;
      float outer = 0.0f;
      if (document_.tag_name(child) == "br") continue;  // zero-width in a max-content measure

      if (document_.tag_name(child) == "img" || document_.tag_name(child) == "canvas") {
        unsigned long long width = 0;
        if (parse_u64(document_.attribute(child, "width"), width)) outer = static_cast<float>(width);
        if (outer <= 0.0f) outer = document_.tag_name(child) == "canvas" ? 300.0f : 120.0f;
      } else {
        outer = intrinsic_width(child) +
                resolve(style.padding[1], style.font_size, viewport_.width) +
                resolve(style.padding[3], style.font_size, viewport_.width) + style.border_width[1] +
                style.border_width[3] + resolve(style.margin[1], style.font_size, viewport_.width) +
                resolve(style.margin[3], style.font_size, viewport_.width);
      }
      const bool inline_level = style.display == css::Display::Inline || style.display == css::Display::InlineBlock;
      if (inline_level) {
        line += outer;
      } else {
        best = std::max(best, line);
        line = 0.0f;
        best = std::max(best, outer);
      }
    }
    return std::max(best, line);
  }

  [[nodiscard]] const image::Image* replaced_image(dom::NodeId node) const {
    // <canvas> bitmaps reach us through the viewport: the bindings layer fills
    // them, layout only needs the pixels.
    if (viewport_.canvases != nullptr) {
      for (const CanvasBitmap& canvas : *viewport_.canvases) {
        if (canvas.node == node && canvas.image != nullptr) return canvas.image;
      }
    }
    return nullptr;
  }

  /// Intrinsic size of a replaced element (`<img>`, `<canvas>`): width/height
  /// attributes first, then the bitmap, then the placeholder defaults.
  void replaced_intrinsic_size(dom::NodeId node, float& width, float& height) const {
    width = 0.0f;
    height = 0.0f;
    unsigned long long attribute_width = 0;
    unsigned long long attribute_height = 0;
    if (parse_u64(document_.attribute(node, "width"), attribute_width)) {
      width = static_cast<float>(attribute_width);
    }
    if (parse_u64(document_.attribute(node, "height"), attribute_height)) {
      height = static_cast<float>(attribute_height);
    }
    const bool is_canvas = document_.tag_name(node) == "canvas";
    const image::Image* bitmap =
        is_canvas ? replaced_image(node)
                  : (images_ == nullptr ? nullptr : images_->find(document_.attribute(node, "src")));
    if (bitmap != nullptr) {
      if (width <= 0.0f) width = static_cast<float>(bitmap->width);
      if (height <= 0.0f) height = static_cast<float>(bitmap->height);
    }
    if (is_canvas) {
      if (width <= 0.0f) width = 300.0f;  // HTML default canvas size
      if (height <= 0.0f) height = 150.0f;
    } else {
      if (width <= 0.0f) width = std::min(120.0f, viewport_.width / 2.0f);
      if (height <= 0.0f) height = width * 0.6f;
    }
  }

  [[nodiscard]] bool is_replaced(dom::NodeId node) const {
    const std::string_view tag = document_.tag_name(node);
    return tag == "img" || tag == "canvas";
  }

  void init_child_box(Box& box, dom::NodeId node, const css::ComputedStyle& style, float x, float y,
                      float border_width) {
    box.kind = BoxKind::Block;
    box.node = node;
    box.style = &style;
    compute_edges(box, style, border_width);
    const float horizontal = box.border[1] + box.border[3] + box.padding[1] + box.padding[3];
    box.border_box = Rect{x, y, border_width, 0.0f};
    box.content_box = Rect{x + box.border[3] + box.padding[3], y + box.border[0] + box.padding[0],
                           std::max(0.0f, border_width - horizontal), 0.0f};
  }

  /// Lays out `box`'s content and returns the used content height.
  float layout_content(Box& box, float width) {
    const css::ComputedStyle& style = *box.style;
    if (style.display == css::Display::Flex) return layout_flex(box, width);
    if (style.display == css::Display::Grid) return layout_grid(box, width);
    if (style.display == css::Display::TableRow) return layout_table_row(box, width);
    if (has_block_children(box.node)) {
      return layout_block_children(box, box.content_box.x, box.content_box.y, width);
    }
    return layout_inline(box, box.content_box.x, box.content_box.y, width);
  }

  struct FlexItem {
    dom::NodeId node;
    const css::ComputedStyle* style = nullptr;
    Box box;
    float margin_main[2] = {0.0f, 0.0f};   // before/after on the main axis
    float margin_cross[2] = {0.0f, 0.0f};  // before/after on the cross axis
    float base = 0.0f;                     // main size of the border box (before flexing)
    float main = 0.0f;                     // final main size
    float cross = 0.0f;                    // final cross size
    float pad_cross = 0.0f;                // padding + border on the cross axis
    std::size_t line = 0;
  };

  float layout_flex(Box& container, float content_width) {
    const css::ComputedStyle& style = *container.style;
    const bool row = style.flex_direction == css::FlexDirection::Row ||
                     style.flex_direction == css::FlexDirection::RowReverse;
    const bool column = !row;
    const bool reverse_main = style.flex_direction == css::FlexDirection::RowReverse ||
                              style.flex_direction == css::FlexDirection::ColumnReverse;
    const bool wrap = style.flex_wrap != css::FlexWrap::NoWrap;
    const float gap = style.gap;

    std::vector<FlexItem> items;
    for (const dom::NodeId child : document_.children(container.node)) {
      if (document_.type(child) != dom::NodeType::Element) continue;
      const css::ComputedStyle& child_style = styles_.get(document_, child);
      if (child_style.display == css::Display::None) continue;
      if (child_style.is_out_of_flow()) continue;
      FlexItem item;
      item.node = child;
      item.style = &child_style;

      const float pad_main = row ? resolve(child_style.padding[3], child_style.font_size, content_width) +
                                       resolve(child_style.padding[1], child_style.font_size, content_width)
                                 : resolve(child_style.padding[0], child_style.font_size, content_width) +
                                       resolve(child_style.padding[2], child_style.font_size, content_width);
      const float pad_cross = row ? resolve(child_style.padding[0], child_style.font_size, content_width) +
                                        resolve(child_style.padding[2], child_style.font_size, content_width)
                                  : resolve(child_style.padding[3], child_style.font_size, content_width) +
                                        resolve(child_style.padding[1], child_style.font_size, content_width);
      const float border_main = row ? child_style.border_width[1] + child_style.border_width[3]
                                    : child_style.border_width[0] + child_style.border_width[2];
      const float border_cross = row ? child_style.border_width[0] + child_style.border_width[2]
                                     : child_style.border_width[1] + child_style.border_width[3];
      item.margin_main[0] = resolve(row ? child_style.margin[3] : child_style.margin[0], child_style.font_size,
                                    content_width);
      item.margin_main[1] = resolve(row ? child_style.margin[1] : child_style.margin[2], child_style.font_size,
                                    content_width);
      item.margin_cross[0] = resolve(row ? child_style.margin[0] : child_style.margin[3], child_style.font_size,
                                     content_width);
      item.margin_cross[1] = resolve(row ? child_style.margin[2] : child_style.margin[1], child_style.font_size,
                                     content_width);

      // Base main size: flex-basis, then the specified size, then max-content.
      float content_main = 0.0f;
      if (!child_style.flex_basis.is_auto()) {
        content_main = resolve(child_style.flex_basis, child_style.font_size, content_width);
      } else if (row && !child_style.width.is_auto()) {
        content_main = resolve(child_style.width, child_style.font_size, content_width);
      } else if (column && !child_style.height.is_auto()) {
        content_main = resolve(child_style.height, child_style.font_size, content_width);
      } else if (row) {
        content_main = std::min(intrinsic_width(child), content_width);
      } else {
        Box probe;  // column with auto height: measure by laying the item out
        init_child_box(probe, child, child_style, 0.0f, 0.0f, content_width);
        content_main = layout_content(probe, probe.content_box.width);
      }
      item.base = std::max(0.0f, content_main) + pad_main + border_main;
      item.main = item.base;
      item.pad_cross = pad_cross + border_cross;
      items.push_back(std::move(item));
    }
    if (items.empty()) return 0.0f;

    // --- break into lines ---------------------------------------------------
    std::size_t line = 0;
    float cursor = 0.0f;
    for (FlexItem& item : items) {
      const float outer = item.main + item.margin_main[0] + item.margin_main[1];
      if (wrap && cursor > 0.0f && cursor + gap + outer > content_width + 0.01f) {
        ++line;
        cursor = 0.0f;
      }
      item.line = line;
      cursor += (cursor > 0.0f ? gap : 0.0f) + outer;
    }
    const std::size_t line_count = line + 1;

    // --- per line: flexing, justification, measurement ----------------------
    std::vector<float> main_position(items.size(), 0.0f);
    std::vector<float> line_cross(line_count, 0.0f);
    std::vector<float> line_offset(line_count, 0.0f);

    for (std::size_t current = 0; current < line_count; ++current) {
      std::vector<std::size_t> indices;
      float used = 0.0f;
      float total_grow = 0.0f;
      float total_shrink = 0.0f;
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].line != current) continue;
        indices.push_back(index);
        used += items[index].main + items[index].margin_main[0] + items[index].margin_main[1];
        total_grow += items[index].style->flex_grow;
        total_shrink += items[index].style->flex_shrink;
      }
      if (indices.size() > 1) used += gap * static_cast<float>(indices.size() - 1);
      const float free = content_width - used;
      if (free > 0.0f && total_grow > 0.0f) {
        const float start_position = 0.0f;
        (void)start_position;
        for (const std::size_t index : indices) {
          items[index].main += free * items[index].style->flex_grow / total_grow;
        }
      } else if (free < 0.0f && total_shrink > 0.0f) {
        for (const std::size_t index : indices) {
          const float share = -free * items[index].style->flex_shrink / total_shrink;
          items[index].main = std::max(0.0f, items[index].main - share);
        }
      }

      float remaining = content_width - (indices.size() > 1 ? gap * static_cast<float>(indices.size() - 1) : 0.0f);
      for (const std::size_t index : indices) {
        remaining -= items[index].main + items[index].margin_main[0] + items[index].margin_main[1];
      }
      remaining = std::max(0.0f, remaining);
      float offset = 0.0f;
      float between = gap;
      switch (style.justify_content) {
        case css::JustifyContent::Center:
          offset = remaining / 2.0f;
          break;
        case css::JustifyContent::FlexEnd:
          offset = remaining;
          break;
        case css::JustifyContent::SpaceBetween:
          if (indices.size() > 1) between = gap + remaining / static_cast<float>(indices.size() - 1);
          break;
        case css::JustifyContent::SpaceAround: {
          const float unit = indices.empty() ? 0.0f : remaining / static_cast<float>(indices.size());
          offset = unit / 2.0f;
          between = gap + unit;
          break;
        }
        case css::JustifyContent::SpaceEvenly: {
          const float unit = remaining / static_cast<float>(indices.size() + 1);
          offset = unit;
          between = gap + unit;
          break;
        }
        case css::JustifyContent::FlexStart:
          break;
      }

      float position = offset;
      for (const std::size_t index : indices) {
        FlexItem& item = items[index];
        position += item.margin_main[0];
        main_position[index] = position;
        position += item.main + item.margin_main[1] + between;

        // Measure the cross size with a probe layout (positions are re-laid later).
        Box probe;
        if (row) {
          init_child_box(probe, item.node, *item.style, 0.0f, 0.0f, item.main);
          const float content_height = layout_content(probe, probe.content_box.width);
          // A specified cross size always wins; `align-items: stretch` only feeds
          // items whose height is auto (CSS Flexbox §9.4).
          item.cross = content_height + item.pad_cross;
          if (!item.style->height.is_auto()) {
            item.cross = resolve(item.style->height, item.style->font_size, content_width) + item.pad_cross;
          }
        } else {
          init_child_box(probe, item.node, *item.style, 0.0f, 0.0f,
                         std::max(0.0f, content_width - item.margin_cross[0] - item.margin_cross[1]));
          const float content_height = layout_content(probe, probe.content_box.width);
          item.cross = std::max(item.main, content_height + item.pad_cross);
          if (!item.style->height.is_auto()) {
            item.cross = resolve(item.style->height, item.style->font_size, content_width) + item.pad_cross;
          }
        }
        line_cross[current] = std::max(line_cross[current], item.cross);
      }
    }

    float cross_cursor = 0.0f;
    for (std::size_t current = 0; current < line_count; ++current) {
      line_offset[current] = cross_cursor;
      cross_cursor += line_cross[current] + (current + 1 < line_count ? gap : 0.0f);
    }

    // --- place and lay out for real -----------------------------------------
    const float total_main = row ? content_width : cross_cursor;
    for (FlexItem& item : items) {
      float x = 0.0f;
      float y = 0.0f;
      float width = 0.0f;
      float border_height = 0.0f;

      if (row) {
        const float main_pos = reverse_main ? (total_main - (main_position[static_cast<std::size_t>(&item - items.data())] + item.main)) : main_position[static_cast<std::size_t>(&item - items.data())];
        const float line_size = line_cross[item.line];
        float cross_offset = 0.0f;
        switch (style.align_items) {
          case css::AlignItems::Center:
            cross_offset = (line_size - item.cross) / 2.0f;
            break;
          case css::AlignItems::FlexEnd:
            cross_offset = line_size - item.cross;
            break;
          case css::AlignItems::FlexStart:
          case css::AlignItems::Stretch:
            break;
        }
        x = container.content_box.x + main_pos + item.margin_main[0];
        y = container.content_box.y + line_offset[item.line] + cross_offset + item.margin_cross[0];
        width = item.main;

        Box box;
        init_child_box(box, item.node, *item.style, x, y, width);
        const float content_height = layout_content(box, box.content_box.width);
        float final_content_height = content_height;
        if (!item.style->height.is_auto()) {
          // `height` is the content-box height, which is what the item asked for.
          final_content_height = resolve(item.style->height, item.style->font_size, content_width);
        } else if (style.align_items == css::AlignItems::Stretch) {
          final_content_height = std::max(content_height, line_size - item.pad_cross);
        }
        box.content_box.height = final_content_height;
        border_height = final_content_height + item.pad_cross;
        box.border_box.height = border_height;
        container.children.push_back(std::move(box));
      } else {
        const float main_pos = reverse_main
                                   ? (total_main - (main_position[static_cast<std::size_t>(&item - items.data())] + item.main))
                                   : main_position[static_cast<std::size_t>(&item - items.data())];
        const float line_size = line_cross[item.line];
        x = container.content_box.x + item.margin_cross[0];
        y = container.content_box.y + line_offset[item.line] + main_pos + item.margin_main[0];
        width = std::max(0.0f, content_width - item.margin_cross[0] - item.margin_cross[1]);

        Box box;
        init_child_box(box, item.node, *item.style, x, y, width);
        const float content_height = layout_content(box, box.content_box.width);
        box.content_box.height = std::max(content_height, item.main - (item.pad_cross > 0.0f ? item.pad_cross : 0.0f));
        box.border_box.height = std::max(item.main, box.content_box.height + item.pad_cross);
        border_height = box.border_box.height;
        (void)line_size;
        container.children.push_back(std::move(box));
      }
      (void)border_height;
    }
    return cross_cursor;
  }

  // --- tables -------------------------------------------------------------
  /// One table row: cells sit side by side, auto widths share the leftover space,
  /// and every cell is stretched to the tallest cell in the row. This is the
  /// "anonymous table" simplification: no column model, no borders collapsing.
  float layout_table_row(Box& row, float content_width) {
    std::vector<dom::NodeId> cells;
    float fixed_width = 0.0f;
    std::size_t automatic = 0;
    for (const dom::NodeId child : document_.children(row.node)) {
      if (document_.type(child) != dom::NodeType::Element) continue;
      const css::ComputedStyle& child_style = styles_.get(document_, child);
      if (child_style.display == css::Display::None) continue;
      if (child_style.is_out_of_flow()) continue;
      cells.push_back(child);
      if (child_style.width.is_auto()) {
        ++automatic;
      } else {
        const float horizontal = resolve(child_style.padding[3], child_style.font_size, content_width) +
                                 resolve(child_style.padding[1], child_style.font_size, content_width) +
                                 child_style.border_width[1] + child_style.border_width[3];
        fixed_width += resolve(child_style.width, child_style.font_size, content_width) + horizontal;
      }
    }
    if (cells.empty()) return 0.0f;

    const float automatic_width =
        automatic == 0 ? 0.0f : std::max(0.0f, (content_width - fixed_width) / static_cast<float>(automatic));
    float x = row.content_box.x;
    float row_height = 0.0f;
    std::vector<Box> boxes;
    for (const dom::NodeId cell : cells) {
      const css::ComputedStyle& cell_style = styles_.get(document_, cell);
      const float horizontal = resolve(cell_style.padding[3], cell_style.font_size, content_width) +
                               resolve(cell_style.padding[1], cell_style.font_size, content_width) +
                               cell_style.border_width[1] + cell_style.border_width[3];
      const float width = cell_style.width.is_auto() ? automatic_width
                                                     : resolve(cell_style.width, cell_style.font_size, content_width) +
                                                           horizontal;
      Box cell_box;
      init_child_box(cell_box, cell, cell_style, x, row.content_box.y, width);
      float height = layout_content(cell_box, cell_box.content_box.width);
      if (!cell_style.height.is_auto()) height = resolve(cell_style.height, cell_style.font_size, content_width);
      cell_box.content_box.height = height;
      cell_box.border_box.height = height + cell_box.padding[0] + cell_box.padding[2] + cell_box.border[0] +
                                   cell_box.border[2];
      row_height = std::max(row_height, cell_box.border_box.height);
      x += cell_box.border_box.width;
      boxes.push_back(std::move(cell_box));
    }
    for (Box& cell_box : boxes) {  // stretch every cell to the row height
      const float vertical = cell_box.padding[0] + cell_box.padding[2] + cell_box.border[0] + cell_box.border[2];
      cell_box.border_box.height = std::max(cell_box.border_box.height, row_height);
      cell_box.content_box.height = std::max(0.0f, cell_box.border_box.height - vertical);
      row.children.push_back(std::move(cell_box));
    }
    return row_height;
  }

  // --- grid ---------------------------------------------------------------
  static float resolve_track(const css::GridTrack& track, float content_width) {
    switch (track.unit) {
      case css::GridTrack::Unit::Px:
        return track.value;
      case css::GridTrack::Unit::Percent:
        return track.value * content_width / 100.0f;
      case css::GridTrack::Unit::Fr:
      case css::GridTrack::Unit::Auto:
        return 0.0f;  // filled in later
    }
    return 0.0f;
  }

  float layout_grid(Box& container, float content_width) {
    const css::ComputedStyle& style = *container.style;
    std::vector<dom::NodeId> cells;
    for (const dom::NodeId child : document_.children(container.node)) {
      if (document_.type(child) != dom::NodeType::Element) continue;
      if (styles_.get(document_, child).display == css::Display::None) continue;
      if (styles_.get(document_, child).is_out_of_flow()) continue;
      cells.push_back(child);
    }
    if (cells.empty()) return 0.0f;

    const float column_gap = style.column_gap > 0.0f ? style.column_gap : style.gap;
    const float row_gap = style.row_gap > 0.0f ? style.row_gap : style.gap;

    std::vector<float> columns;
    if (style.grid_columns.empty()) {
      columns.push_back(content_width);
    } else {
      float fixed = 0.0f;
      float fraction_sum = 0.0f;
      std::size_t auto_count = 0;
      for (const css::GridTrack& track : style.grid_columns) {
        if (track.unit == css::GridTrack::Unit::Px || track.unit == css::GridTrack::Unit::Percent) {
          fixed += resolve_track(track, content_width);
        } else if (track.unit == css::GridTrack::Unit::Fr) {
          fraction_sum += track.value;
        } else {
          ++auto_count;
        }
      }
      const float available =
          std::max(0.0f, content_width - fixed - column_gap * static_cast<float>(style.grid_columns.size() - 1));
      const float unit = fraction_sum > 0.0f ? available / fraction_sum : 0.0f;
      const float auto_unit = auto_count > 0 ? available / static_cast<float>(auto_count + fraction_sum) : 0.0f;
      for (const css::GridTrack& track : style.grid_columns) {
        if (track.unit == css::GridTrack::Unit::Fr) {
          columns.push_back(unit * track.value);
        } else if (track.unit == css::GridTrack::Unit::Auto) {
          columns.push_back(auto_unit);
        } else {
          columns.push_back(resolve_track(track, content_width));
        }
      }
    }
    const std::size_t column_count = std::max<std::size_t>(1, columns.size());
    const std::size_t row_count = (cells.size() + column_count - 1) / column_count;

    // Column x positions (grid is left-to-right; the container's width is the limit).
    std::vector<float> column_x(column_count, 0.0f);
    {
      float x = container.content_box.x;
      for (std::size_t column = 0; column < column_count; ++column) {
        column_x[column] = x;
        x += columns[column] + column_gap;
      }
    }

    auto cell_width = [&](std::size_t column) { return columns[column]; };

    // Pass 1: measure each cell's height at its final width.
    std::vector<float> row_heights(row_count, 0.0f);
    std::vector<float> cell_height(cells.size(), 0.0f);
    for (std::size_t index = 0; index < cells.size(); ++index) {
      const std::size_t column = index % column_count;
      const std::size_t row_index = index / column_count;
      const dom::NodeId child = cells[index];
      const css::ComputedStyle& child_style = styles_.get(document_, child);
      const float margin_left = resolve(child_style.margin[3], child_style.font_size, cell_width(column));
      const float margin_right = resolve(child_style.margin[1], child_style.font_size, cell_width(column));
      const float width = std::max(0.0f, cell_width(column) - margin_left - margin_right);
      Box probe;
      init_child_box(probe, child, child_style, column_x[column] + margin_left, 0.0f, width);
      const float measured = layout_content(probe, probe.content_box.width);
      float height = measured + probe.padding[0] + probe.padding[2] + probe.border[0] + probe.border[2];
      if (!child_style.height.is_auto()) {
        height = resolve(child_style.height, child_style.font_size, content_width) + probe.padding[0] +
                 probe.padding[2] + probe.border[0] + probe.border[2];
      }
      cell_height[index] = height;
      row_heights[row_index] = std::max(row_heights[row_index], height);
    }

    // Explicit row tracks win over measured heights.
    if (!style.grid_rows.empty()) {
      for (std::size_t row_index = 0; row_index < row_heights.size() && row_index < style.grid_rows.size();
           ++row_index) {
        const css::GridTrack& track = style.grid_rows[row_index];
        if (track.unit == css::GridTrack::Unit::Px || track.unit == css::GridTrack::Unit::Percent) {
          row_heights[row_index] = resolve_track(track, content_width);
        }
      }
    }

    // Pass 2: place and lay out at the final position.
    float y = container.content_box.y;
    for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
      for (std::size_t column = 0; column < column_count; ++column) {
        const std::size_t index = row_index * column_count + column;
        if (index >= cells.size()) break;
        const dom::NodeId child = cells[index];
        const css::ComputedStyle& child_style = styles_.get(document_, child);
        const float margin_left = resolve(child_style.margin[3], child_style.font_size, cell_width(column));
        const float margin_right = resolve(child_style.margin[1], child_style.font_size, cell_width(column));
        const float width = std::max(0.0f, cell_width(column) - margin_left - margin_right);
        Box box;
        init_child_box(box, child, child_style, column_x[column] + margin_left, y, width);
        const float measured = layout_content(box, box.content_box.width);
        const float extras = box.padding[0] + box.padding[2] + box.border[0] + box.border[2];
        if (!child_style.height.is_auto()) {
          box.content_box.height = resolve(child_style.height, child_style.font_size, content_width);
        } else if (row_heights[row_index] > measured + extras) {
          // Stretch to the row height when the item has an auto height.
          box.content_box.height = row_heights[row_index] - extras;
        } else {
          box.content_box.height = measured;
        }
        box.border_box.height = box.content_box.height + extras;
        container.children.push_back(std::move(box));
      }
      y += row_heights[row_index] + row_gap;
    }
    return std::max(0.0f, y - container.content_box.y - (row_count > 0 ? row_gap : 0.0f));
  }

  // --- floats (stage 21) -------------------------------------------------

  /// An inline-block, an absolutely positioned box or a flex container starts a new
  /// block formatting context: floats from the outside do not reach its contents.
  class FloatGuard {
   public:
    explicit FloatGuard(std::vector<FloatRect>& floats) : floats_(floats), saved_(std::move(floats)) {
      floats_.clear();
    }
    ~FloatGuard() { floats_ = std::move(saved_); }
    FloatGuard(const FloatGuard&) = delete;
    FloatGuard& operator=(const FloatGuard&) = delete;

   private:
    std::vector<FloatRect>& floats_;
    std::vector<FloatRect> saved_;
  };

  /// Horizontal span left for a line box or a float at [y, y + height).
  void span_at(float content_x, float content_width, float y, float height, float& left, float& right) const {
    left = content_x;
    right = content_x + content_width;
    for (const FloatRect& float_box : floats_) {
      if (float_box.rect.bottom() <= y + 0.01f || float_box.rect.y >= y + height - 0.01f) continue;
      if (float_box.side == css::FloatSide::Left) {
        left = std::max(left, float_box.rect.right());
      } else {
        right = std::min(right, float_box.rect.x);
      }
    }
    if (right < left) right = left;
  }

  /// Smallest bottom edge of a float that still crosses `y` — where a line box (or a
  /// float that does not fit) has to move to.
  [[nodiscard]] float next_float_bottom(float y) const {
    float best = std::numeric_limits<float>::max();
    for (const FloatRect& float_box : floats_) {
      if (float_box.rect.bottom() > y + 0.01f) best = std::min(best, float_box.rect.bottom());
    }
    return best == std::numeric_limits<float>::max() ? y : best;
  }

  /// `clear` pushes a box below the floats on the given side(s).
  [[nodiscard]] float clearance(css::ClearSide clear, float y) const {
    if (clear == css::ClearSide::None) return y;
    float bottom = y;
    for (const FloatRect& float_box : floats_) {
      if (clear == css::ClearSide::Left && float_box.side != css::FloatSide::Left) continue;
      if (clear == css::ClearSide::Right && float_box.side != css::FloatSide::Right) continue;
      bottom = std::max(bottom, float_box.rect.bottom());
    }
    return bottom;
  }

  /// Lays out one float: shrink-to-fit width against the space that is actually free
  /// at `y`, moved down while it does not fit. Its bottom does not advance the flow —
  /// that is the whole point of floating — but it is recorded for the line boxes and
  /// for `clear`.
  Box place_float(dom::NodeId node, const css::ComputedStyle& style, float content_x, float y,
                  float content_width) {
    Box box;
    box.kind = BoxKind::Block;
    box.node = node;
    box.style = &style;
    compute_edges(box, style, content_width);
    const float horizontal = box.border[1] + box.border[3] + box.padding[1] + box.padding[3];

    // A floated `<img>`/`<canvas>` is sized by its intrinsic box, not by measuring text.
    float replaced_width = 0.0f;
    float replaced_height = 0.0f;
    const bool replaced = is_replaced(node);
    if (replaced) replaced_intrinsic_size(node, replaced_width, replaced_height);

    float left = content_x;
    float right = content_x + content_width;
    float content_used = 0.0f;
    for (int attempt = 0; attempt < 8; ++attempt) {
      span_at(content_x, content_width, y, 0.5f, left, right);
      const float available = right - left;
      float wanted = 0.0f;
      if (!style.width.is_auto()) {
        wanted = resolve(style.width, style.font_size, content_width);
      } else if (replaced) {
        wanted = std::min(replaced_width, std::max(0.0f, available - horizontal));
      } else {
        wanted = std::min(intrinsic_width(node), std::max(0.0f, available - horizontal));
      }
      if (style.min_width > 0.0f) wanted = std::max(wanted, style.min_width);
      if (style.max_width > 0.0f) wanted = std::min(wanted, style.max_width);
      content_used = std::max(0.0f, wanted);
      if (content_used + horizontal <= available + 0.01f) break;
      const float next = next_float_bottom(y);
      if (next <= y + 0.01f) break;  // nothing below to make room: keep the width we have
      y = next;
    }

    const float border_width = content_used + horizontal;
    const float x = style.float_side == css::FloatSide::Right ? std::max(left, right - border_width)
                                                              : left;
    box.border_box = Rect{x, y, border_width, 0.0f};
    box.content_box =
        Rect{x + box.border[3] + box.padding[3], y + box.border[0] + box.padding[0], content_used, 0.0f};

    // The float's own content starts a fresh formatting context: outer floats do not
    // reach into it (and the guard must be gone before the float registers itself).
    const float content_height = [&] {
      FloatGuard guard(floats_);
      return replaced ? replaced_height : layout_content(box, content_used);
    }();
    const float explicit_height = style.height.is_auto() ? -1.0f : resolve(style.height, style.font_size, content_width);
    float total_height = explicit_height >= 0.0f ? explicit_height : content_height;
    total_height = std::max(total_height, style.min_height);
    box.content_box.height = total_height;
    box.border_box.height = total_height + box.padding[0] + box.padding[2] + box.border[0] + box.border[2];

    floats_.push_back(FloatRect{box.border_box, style.float_side});
    return box;
  }

  /// Bottom edge of every float of the current formatting context (the root element
  /// contains its floats even when they overflow their parent).
  [[nodiscard]] float floats_bottom() const {
    float bottom = 0.0f;
    for (const FloatRect& float_box : floats_) bottom = std::max(bottom, float_box.rect.bottom());
    return bottom;
  }

  // --- positioning (stage 20) --------------------------------------------

  /// Shifts a laid-out subtree. Boxes are laid out at absolute coordinates, so a
  /// single delta covers the whole subtree (children included).
  static void translate(Box& box, float dx, float dy) {
    if (dx == 0.0f && dy == 0.0f) return;
    box.border_box.x += dx;
    box.border_box.y += dy;
    box.content_box.x += dx;
    box.content_box.y += dy;
    box.marker_x += dx;
    for (Box& child : box.children) translate(child, dx, dy);
    for (Box& child : box.positioned) translate(child, dx, dy);
  }

  [[nodiscard]] static Rect padding_box(const Box& box) {
    return Rect{box.content_box.x - box.padding[3], box.content_box.y - box.padding[0],
                box.content_box.width + box.padding[1] + box.padding[3],
                box.content_box.height + box.padding[0] + box.padding[2]};
  }

  /// `position: fixed` is always anchored to the viewport, everything else to the
  /// nearest positioned ancestor; with none, to the initial containing block.
  [[nodiscard]] bool is_containing_block(dom::NodeId node, dom::NodeId candidate, bool candidate_is_root) const {
    for (dom::NodeId parent = document_.parent(node); parent.valid(); parent = document_.parent(parent)) {
      if (styles_.get(document_, parent).is_positioned()) return parent == candidate;
    }
    return candidate_is_root;
  }

  /// Collects the out-of-flow descendants whose containing block is `candidate`.
  /// The walk stops at any positioned element: it establishes its own containing
  /// block, and its descendants are placed in its own pass.
  void collect_out_of_flow(dom::NodeId parent, dom::NodeId candidate, bool candidate_is_root,
                           std::vector<dom::NodeId>& out) const {
    for (const dom::NodeId child : document_.children(parent)) {
      if (document_.type(child) != dom::NodeType::Element) continue;
      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;
      if (style.position == css::Position::Fixed) {
        if (candidate_is_root) out.push_back(child);  // the viewport is its containing block
        continue;
      }
      if (style.is_out_of_flow()) {
        if (is_containing_block(child, candidate, candidate_is_root)) out.push_back(child);
        continue;
      }
      if (style.is_positioned()) continue;
      collect_out_of_flow(child, candidate, candidate_is_root, out);
    }
  }

  [[nodiscard]] bool recorded_static_position(dom::NodeId node, float& x, float& y) const {
    for (const StaticPosition& position : static_positions_) {
      if (position.node == node) {
        x = position.x;
        y = position.y;
        return true;
      }
    }
    return false;
  }

  /// Lays out one absolutely positioned box inside `cb` (its containing block).
  /// `box.content_box.y` starts at 0 and is translated into place at the end: the
  /// `bottom` inset depends on the box's own height, which is only known after the
  /// content is laid out.
  Box layout_positioned_box(dom::NodeId node, const Rect& cb) {
    const css::ComputedStyle& style = styles_.get(document_, node);
    Box box;
    box.kind = BoxKind::Block;
    box.node = node;
    box.style = &style;
    box.out_of_flow = true;
    box.fixed = style.position == css::Position::Fixed;
    box.z_index = style.has_z_index ? style.z_index : 0;
    compute_edges(box, style, cb.width);

    const float horizontal = box.border[1] + box.border[3] + box.padding[1] + box.padding[3];
    const float left = style.inset[3].is_auto() ? -1.0f : resolve(style.inset[3], style.font_size, cb.width);
    const float right = style.inset[1].is_auto() ? -1.0f : resolve(style.inset[1], style.font_size, cb.width);

    float content_width = 0.0f;
    if (!style.width.is_auto()) {
      content_width = resolve(style.width, style.font_size, cb.width);
    } else if (left >= 0.0f && right >= 0.0f) {
      // Both insets are given: the box is stretched to whatever is left between them.
      content_width = cb.width - left - right - box.margin[1] - box.margin[3] - horizontal;
    } else {
      // Shrink-to-fit: as wide as the content wants, but never wider than the space
      // between the used inset and the far edge of the containing block.
      const float used_left = left >= 0.0f ? left : 0.0f;
      const float used_right = right >= 0.0f ? right : 0.0f;
      const float available =
          cb.width - used_left - used_right - box.margin[1] - box.margin[3] - horizontal;
      content_width = std::min(intrinsic_width(node) + horizontal, available) - horizontal;
    }
    if (style.min_width > 0.0f) content_width = std::max(content_width, style.min_width);
    if (style.max_width > 0.0f) content_width = std::min(content_width, style.max_width);
    content_width = std::max(0.0f, content_width);

    const float border_width = content_width + horizontal;
    float static_x = cb.x;
    float static_y = cb.y;
    if (!recorded_static_position(node, static_x, static_y)) {
      static_x = cb.x;  // not a block-level child: fall back to the containing block origin
      static_y = cb.y;
    }

    float x = static_x;
    if (left >= 0.0f) {
      x = cb.x + left + box.margin[3];
    } else if (right >= 0.0f) {
      x = cb.right() - right - box.margin[1] - border_width;
    }

    box.border_box = Rect{x, 0.0f, border_width, 0.0f};
    box.content_box =
        Rect{x + box.border[3] + box.padding[3], box.border[0] + box.padding[0], content_width, 0.0f};
    FloatGuard guard(floats_);  // an absolutely positioned box is a formatting context
    const float content_height = layout_content(box, content_width);

    const float explicit_height =
        style.height.is_auto() ? -1.0f : resolve(style.height, style.font_size, cb.height);
    float total_height = explicit_height >= 0.0f ? explicit_height : content_height;
    total_height = std::max(total_height, style.min_height);
    box.content_box.height = total_height;
    box.border_box.height = total_height + box.padding[0] + box.padding[2] + box.border[0] + box.border[2];

    const float top = style.inset[0].is_auto() ? -1.0f : resolve(style.inset[0], style.font_size, cb.height);
    const float bottom = style.inset[2].is_auto() ? -1.0f : resolve(style.inset[2], style.font_size, cb.height);
    float y = static_y;
    if (top >= 0.0f) {
      y = cb.y + top + box.margin[0];
    } else if (bottom >= 0.0f) {
      y = cb.bottom() - bottom - box.margin[2] - box.border_box.height;
    }
    translate(box, 0.0f, y);
    return box;
  }

  /// `position: relative` keeps the box in the flow but shifts it (and its subtree)
  /// by the insets; siblings stay where they were.
  void apply_relative_offsets(Box& box) {
    for (Box& child : box.children) {
      if (child.style != nullptr && child.style->position == css::Position::Relative) {
        const css::ComputedStyle& style = *child.style;
        float dx = 0.0f;
        float dy = 0.0f;
        // Percentages resolve against the containing block: the parent's content box.
        if (!style.inset[3].is_auto()) {
          dx = resolve(style.inset[3], style.font_size, box.content_box.width);
        } else if (!style.inset[1].is_auto()) {
          dx = -resolve(style.inset[1], style.font_size, box.content_box.width);
        }
        if (!style.inset[0].is_auto()) {
          dy = resolve(style.inset[0], style.font_size, box.content_box.height);
        } else if (!style.inset[2].is_auto()) {
          dy = -resolve(style.inset[2], style.font_size, box.content_box.height);
        }
        translate(child, dx, dy);
      }
      apply_relative_offsets(child);
    }
    for (Box& child : box.positioned) apply_relative_offsets(child);
  }

  /// Places every out-of-flow box whose containing block is `box`, then hands the
  /// subtree to `descend_positioned()`: containing blocks further down get their own
  /// pass (the nearest positioned ancestor owns an out-of-flow box, not the root).
  void place_positioned(Box& box, bool is_root) {
    std::vector<dom::NodeId> out_of_flow;
    collect_out_of_flow(box.node, box.node, is_root, out_of_flow);
    if (!out_of_flow.empty()) {
      const Rect cb = is_root ? Rect{0.0f, 0.0f, viewport_.width, viewport_.height} : padding_box(box);
      for (const dom::NodeId node : out_of_flow) {
        Box child = layout_positioned_box(node, cb);
        place_positioned(child, false);  // it is a containing block for its descendants
        box.positioned.push_back(std::move(child));
      }
    }
    for (Box& child : box.children) descend_positioned(child);
  }

  /// Walks the in-flow tree until it reaches a box that establishes a containing
  /// block; that box places its own out-of-flow descendants.
  void descend_positioned(Box& box) {
    if (box.style != nullptr && box.style->is_positioned()) {
      place_positioned(box, false);
      return;
    }
    for (Box& child : box.children) descend_positioned(child);
    // A box that is not positioned has no `positioned` children of its own.
  }

  /// Absolute boxes below the flow still make the page scrollable; `fixed` ones do
  /// not (they are anchored to the viewport).
  [[nodiscard]] float positioned_bottom(const Box& box) const {
    float bottom = 0.0f;
    for (const Box& child : box.positioned) {
      if (child.fixed) continue;
      bottom = std::max(bottom, child.border_box.bottom());
      bottom = std::max(bottom, positioned_bottom(child));
    }
    for (const Box& child : box.children) bottom = std::max(bottom, positioned_bottom(child));
    return bottom;
  }

  // --- block content -----------------------------------------------------
  float layout_block_children(Box& box, float content_x, float start_y, float content_width) {
    float y = start_y;
    bool previous_was_block = false;
    float previous_margin_bottom = 0.0f;

    // Consecutive inline-level siblings form an anonymous block. Flushing them
    // in document order (instead of "all inline content after the blocks") is
    // what makes forms, labels and `text <b>x</b> <input>` look right.
    std::vector<dom::NodeId> inline_run;
    const auto flush_inline_run = [&]() {
      if (inline_run.empty()) return;
      std::vector<InlineItem> items;
      collect_inline_items_from(inline_run, items);
      inline_run.clear();
      if (items.empty()) return;
      y += layout_inline_items(box, content_x, y, content_width, items);
      previous_was_block = true;  // an anonymous block was emitted
      previous_margin_bottom = 0.0f;
    };

    for (const dom::NodeId child : document_.children(box.node)) {
      const dom::NodeType type = document_.type(child);
      if (type == dom::NodeType::Text) {
        inline_run.push_back(child);
        continue;
      }
      if (type != dom::NodeType::Element) continue;

      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;
      // Out of flow: remembered for the positioning pass, skipped by the flow.
      if (style.is_out_of_flow()) {
        static_positions_.push_back(StaticPosition{child, content_x, y});
        continue;
      }

      if (style.is_floating() && is_block_level(style)) {
        // A float leaves the flow: the following line boxes wrap around it instead of
        // being pushed down.
        flush_inline_run();
        y = std::max(y, clearance(style.clear, y));
        box.children.push_back(place_float(child, style, content_x, y, content_width));
        previous_was_block = true;
        previous_margin_bottom = 0.0f;
        continue;
      }

      if (is_block_level(style) && style.display != css::Display::InlineBlock) {
        flush_inline_run();
        // `clear` pushes the block below the floats on that side.
        y = std::max(y, clearance(style.clear, y));
        Box child_box;
        child_box.kind = BoxKind::Block;
        child_box.node = child;
        child_box.style = &style;
        compute_edges(child_box, style, content_width);

        const float margin_top = child_box.margin[0];
        const float collapsed = previous_was_block ? std::max(previous_margin_bottom, margin_top) : margin_top;
        y += collapsed;

        float outer_width = content_width - child_box.margin[1] - child_box.margin[3];
        float specified_width = -1.0f;
        if (!style.width.is_auto()) {
          specified_width = resolve(style.width, style.font_size, content_width);
        }
        const float padding_and_border = child_box.padding[1] + child_box.padding[3] + child_box.border[1] +
                                         child_box.border[3];
        float border_width = specified_width >= 0.0f ? specified_width + padding_and_border : outer_width;
        border_width = std::max(0.0f, std::min(border_width, outer_width));

        const float border_x = content_x + child_box.margin[3];
        child_box.border_box = Rect{border_x, y, border_width, 0.0f};
        child_box.content_box = Rect{border_x + child_box.border[3] + child_box.padding[3],
                                     y + child_box.border[0] + child_box.padding[0],
                                     std::max(0.0f, border_width - padding_and_border), 0.0f};

        float inner_width = child_box.content_box.width;
        if (style.display == css::Display::ListItem) inner_width = std::max(0.0f, inner_width - kListIndent);
        const float content_x_child =
            style.display == css::Display::ListItem ? child_box.content_box.x + kListIndent : child_box.content_box.x;

        float height = 0.0f;
        if (style.display == css::Display::Flex || style.display == css::Display::Grid ||
            style.display == css::Display::TableRow) {
          height = layout_content(child_box, inner_width);
        } else if (has_block_children(child)) {
          // Mixed content: layout_block_children() already appends the element's
          // loose inline content after its block children (see the comment there).
          height = layout_block_children(child_box, content_x_child, child_box.content_box.y, inner_width);
        } else {
          height = layout_inline(child_box, content_x_child, child_box.content_box.y, inner_width);
        }

        float explicit_height = -1.0f;
        if (!style.height.is_auto()) explicit_height = resolve(style.height, style.font_size, content_width);
        // `height` sets the content box: content taller than that overflows visibly
        // (we have no overflow clipping yet), but the box itself keeps its size.
        float total_height = explicit_height >= 0.0f ? explicit_height : height;
        total_height = std::max(total_height, style.min_height);

        child_box.content_box.height = total_height;
        child_box.border_box.height =
            total_height + child_box.padding[0] + child_box.padding[2] + child_box.border[0] + child_box.border[2];

        if (style.display == css::Display::ListItem) {
          child_box.has_marker = true;
          child_box.marker_x = child_box.content_box.x - kListIndent + 4.0f;
        }

        y = child_box.border_box.bottom();
        previous_margin_bottom = child_box.margin[2];
        previous_was_block = true;
        box.children.push_back(std::move(child_box));
        continue;
      }

      // Inline-level sibling: part of the current anonymous block.
      inline_run.push_back(child);
    }

    flush_inline_run();
    return y - start_y;
  }

  [[nodiscard]] bool has_block_children(dom::NodeId node) const {
    for (const dom::NodeId child : document_.children(node)) {
      if (document_.type(child) != dom::NodeType::Element) continue;
      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;
      // Out-of-flow children are not part of the flow: they cannot be "block children".
      if (style.is_out_of_flow()) continue;
      if (is_block_level(style) && style.display != css::Display::InlineBlock) return true;
    }
    return false;
  }

  const dom::Document& document_;
  const css::StyleMap& styles_;
  const text::Font& font_;
  LayoutViewport viewport_;
  const image::ImageStore* images_ = nullptr;
  std::vector<StaticPosition> static_positions_;
  /// Floats of the current block formatting context (see `FloatGuard`).
  std::vector<FloatRect> floats_;
};

}  // namespace

namespace {

/// `fixed` boxes are laid out in viewport space; everything else in document space.
const Box* hit_test_box(const Box& box, float x, float y, float scroll_y, bool in_fixed) {
  const bool is_fixed = in_fixed || box.fixed;
  const float point_y = is_fixed ? y - scroll_y : y;
  // Positioned boxes paint above the flow, so they are hit first — from the highest
  // z-index down, which is the reverse of the paint order.
  if (!box.positioned.empty()) {
    std::vector<const Box*> stacked;
    stacked.reserve(box.positioned.size());
    for (const Box& child : box.positioned) stacked.push_back(&child);
    std::stable_sort(stacked.begin(), stacked.end(),
                     [](const Box* a, const Box* b) { return a->z_index < b->z_index; });
    for (auto it = stacked.rbegin(); it != stacked.rend(); ++it) {
      if (!(*it)->border_box.contains(x, point_y)) continue;
      if (const Box* deeper = hit_test_box(**it, x, y, scroll_y, is_fixed)) return deeper;
      return *it;
    }
  }
  for (const Box& child : box.children) {
    if (child.kind == BoxKind::Block && child.border_box.contains(x, point_y)) {
      if (const Box* deeper = hit_test_box(child, x, y, scroll_y, is_fixed)) return deeper;
      return &child;
    }
    if (child.border_box.contains(x, point_y)) return &child;
  }
  return box.border_box.contains(x, point_y) ? &box : nullptr;
}

}  // namespace

const Box* hit_test(const Box& root, float x, float y, float scroll_y) {
  return hit_test_box(root, x, y, scroll_y, false);
}

LayoutResult layout_document(const dom::Document& document, const css::StyleMap& styles, const text::Font& font,
                             const LayoutViewport& viewport, const image::ImageStore* images) {
  LayoutEngine engine(document, styles, font, viewport, images);
  return engine.run();
}

}  // namespace bonk::layout
