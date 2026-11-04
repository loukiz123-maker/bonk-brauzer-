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
    while (index < items.size()) {
      // Gather one line.
      std::vector<const InlineItem*> line;
      float line_width = 0.0f;
      float line_height = 0.0f;
      float line_ascent = 0.0f;
      std::size_t last_break = items.size();  // index of the last breakable space

      while (index < items.size()) {
        const InlineItem& item = items[index];
        if (item.kind == InlineItem::Kind::Text && item.text == "\n") {  // <pre> hard break
          ++index;
          break;
        }
        const bool fits = line_width + item.width <= content_width + 0.01f;
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
      if (line_width > content_width && last_break < line.size()) {
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

      // Horizontal alignment.
      float x = content_x;
      const float free_space = std::max(0.0f, content_width - line_width);
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
          (void)layout_content(child, child.content_box.width);
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
          item.cross = content_height + item.pad_cross;
          if (!item.style->height.is_auto() &&
              style.align_items != css::AlignItems::Stretch) {
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
        if (style.align_items == css::AlignItems::Stretch && item.style->height.is_auto()) {
          final_content_height =
              std::max(content_height, line_size - item.pad_cross);
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

      if (is_block_level(style) && style.display != css::Display::InlineBlock) {
        flush_inline_run();
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
        if (style.display == css::Display::Flex || style.display == css::Display::Grid) {
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
      if (is_block_level(style) && style.display != css::Display::InlineBlock) return true;
    }
    return false;
  }

  const dom::Document& document_;
  const css::StyleMap& styles_;
  const text::Font& font_;
  LayoutViewport viewport_;
  const image::ImageStore* images_ = nullptr;
};

}  // namespace

const Box* hit_test(const Box& root, float x, float y) {
  for (const Box& child : root.children) {
    if (child.kind == BoxKind::Block && child.border_box.contains(x, y)) {
      if (const Box* deeper = hit_test(child, x, y); deeper != nullptr) return deeper;
      return &child;
    }
    if (child.border_box.contains(x, y)) return &child;
  }
  return root.border_box.contains(x, y) ? &root : nullptr;
}

LayoutResult layout_document(const dom::Document& document, const css::StyleMap& styles, const text::Font& font,
                             const LayoutViewport& viewport, const image::ImageStore* images) {
  LayoutEngine engine(document, styles, font, viewport, images);
  return engine.run();
}

}  // namespace bonk::layout
