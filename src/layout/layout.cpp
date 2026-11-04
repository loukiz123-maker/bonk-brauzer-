#include "bonk/layout/layout.hpp"

#include <algorithm>
#include <cmath>

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
      case css::Display::Table:
      case css::Display::TableRow:
      case css::Display::TableCell:
        return true;
      default:
        return false;
    }
  }

  // --- inline content ----------------------------------------------------
  void collect_inline_items(dom::NodeId parent, std::vector<InlineItem>& items) {
    for (const dom::NodeId child : document_.children(parent)) {
      const dom::NodeType type = document_.type(child);
      if (type == dom::NodeType::Text) {
        const css::ComputedStyle& style = styles_.get(document_, child);
        append_text_items(child, document_.text_data(child), style, items);
        continue;
      }
      if (type != dom::NodeType::Element) continue;

      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;

      if (document_.tag_name(child) == "img") {
        InlineItem item;
        item.kind = InlineItem::Kind::Atomic;
        item.node = child;
        item.style = &style;

        // Size priority: HTML width/height attributes, then the decoded bitmap's
        // intrinsic size, then a placeholder that keeps the line height sane.
        unsigned long long width_value = 0;
        unsigned long long height_value = 0;
        if (parse_u64(document_.attribute(child, "width"), width_value)) {
          item.width = static_cast<float>(width_value);
        }
        if (parse_u64(document_.attribute(child, "height"), height_value)) {
          item.height = static_cast<float>(height_value);
        }
        const image::Image* bitmap = images_ == nullptr ? nullptr : images_->find(document_.attribute(child, "src"));
        if (bitmap != nullptr) {
          if (item.width <= 0.0f) item.width = static_cast<float>(bitmap->width);
          if (item.height <= 0.0f) item.height = static_cast<float>(bitmap->height);
        }
        if (item.width <= 0.0f) item.width = std::min(120.0f, viewport_.width / 2.0f);
        if (item.height <= 0.0f) item.height = item.width * 0.6f;
        items.push_back(std::move(item));
        continue;
      }

      if (is_block_level(style) || style.display == css::Display::InlineBlock) {
        // A block inside a block: flush inline items first, then lay out as block.
        // (Full CSS would split the inline context; we approximate by ending it.)
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
        if (item->kind == InlineItem::Kind::Atomic) {
          Box child;
          child.kind = document_.tag_name(item->node) == "img" ? BoxKind::Image : BoxKind::InlineBox;
          child.node = item->node;
          child.style = item->style;
          child.border_box = Rect{x, y, item->width, item->height};
          child.content_box = child.border_box;
          child.font_size = item->style->font_size;
          if (images_ != nullptr && document_.tag_name(item->node) == "img") {
            child.image = images_->find(document_.attribute(item->node, "src"));
          }
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

  // --- block content -----------------------------------------------------
  float layout_block_children(Box& box, float content_x, float start_y, float content_width) {
    float y = start_y;
    bool previous_was_block = false;
    float previous_margin_bottom = 0.0f;
    bool has_inline_content = false;

    for (const dom::NodeId child : document_.children(box.node)) {
      const dom::NodeType type = document_.type(child);
      if (type == dom::NodeType::Text) {
        const std::string_view text = document_.text_data(child);
        const bool blank = trim(text).empty() && !styles_.get(document_, child).white_space_pre;
        if (!blank) has_inline_content = true;
        continue;
      }
      if (type != dom::NodeType::Element) continue;

      const css::ComputedStyle& style = styles_.get(document_, child);
      if (style.display == css::Display::None) continue;

      if (is_block_level(style) && style.display != css::Display::InlineBlock) {
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
        if (has_block_children(child)) {
          height = layout_block_children(child_box, content_x_child, child_box.content_box.y, inner_width);
          // inline content mixed with blocks: lay out after the blocks
          height = std::max(height, layout_inline_if_needed(child_box, content_x_child,
                                                            child_box.content_box.y + height, inner_width));
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
        has_inline_content = false;
        box.children.push_back(std::move(child_box));
        continue;
      }

      // Inline-level sibling: it belongs to the parent's inline formatting context,
      // which is laid out once after all block children (see below).
      has_inline_content = true;
    }

    if (has_inline_content) {
      y += layout_inline(box, content_x, y, content_width);
    }
    return y - start_y;
  }

  float layout_inline_if_needed(Box& box, float content_x, float y, float content_width) {
    // Only used when a block has both block children and loose inline content.
    bool has_inline = false;
    for (const dom::NodeId child : document_.children(box.node)) {
      const dom::NodeType type = document_.type(child);
      if (type == dom::NodeType::Text) {
        if (!trim(document_.text_data(child)).empty()) has_inline = true;
        continue;
      }
      if (type == dom::NodeType::Element) {
        const css::ComputedStyle& style = styles_.get(document_, child);
        if (style.display == css::Display::None) continue;
        if (!is_block_level(style) || style.display == css::Display::InlineBlock) has_inline = true;
      }
    }
    if (!has_inline) return 0.0f;
    return layout_inline(box, content_x, y, content_width);
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
