#include "bonk/paint/render.hpp"

#include <algorithm>
#include <vector>

namespace bonk::paint {
namespace {

/// Adds the current scroll offset: everything below is painted in viewport space.
struct Offset {
  float x = 0.0f;
  float y = 0.0f;
};

paint::Rect to_paint_rect(const layout::Rect& rect, const Offset& offset) {
  return Rect{rect.x + offset.x, rect.y + offset.y, rect.width, rect.height};
}

void paint_marker(Canvas& canvas, const layout::Box& box, const text::Font& font, const Offset& offset) {
  if (!box.has_marker || box.style == nullptr) return;
  const float baseline = box.border_box.y + offset.y + box.style->font_size;
  canvas.draw_text(font, "\xE2\x80\xA2", box.marker_x + offset.x, baseline, box.style->font_size * 0.8f,
                   box.style->color);
}

void paint_border(Canvas& canvas, const layout::Box& box, const Offset& offset) {
  if (box.style == nullptr) return;
  const css::Color color = box.style->border_color;
  if (color.a == 0) return;
  const Rect outer = to_paint_rect(box.border_box, offset);
  if (box.border[0] > 0.0f) canvas.fill_rect(Rect{outer.x, outer.y, outer.width, box.border[0]}, color);
  if (box.border[2] > 0.0f) {
    canvas.fill_rect(Rect{outer.x, outer.y + outer.height - box.border[2], outer.width, box.border[2]}, color);
  }
  if (box.border[3] > 0.0f) canvas.fill_rect(Rect{outer.x, outer.y, box.border[3], outer.height}, color);
  if (box.border[1] > 0.0f) {
    canvas.fill_rect(Rect{outer.x + outer.width - box.border[1], outer.y, box.border[1], outer.height}, color);
  }
}

void paint_box(Canvas& canvas, const layout::Box& box, const text::Font& font, const RenderOptions& options,
               const Offset& offset);

/// Paints a box's out-of-flow children in z-index order. Positioned boxes always go
/// above the in-flow content of the box that contains them.
void paint_positioned(Canvas& canvas, const layout::Box& box, const text::Font& font, const RenderOptions& options,
                      const Offset& offset);

/// Paints the content of `box` with its `overflow` clip in force. The box's own
/// background and border are painted by the caller, *before* the clip: `overflow`
/// clips the contents, not the box itself.
void paint_children(Canvas& canvas, const layout::Box& box, const text::Font& font, const RenderOptions& options,
                    const Offset& offset) {
  const Rect previous = canvas.clip();
  if (box.has_clip) {
    const Rect clip = to_paint_rect(box.clip, offset);
    // Intersect with the clip already in force so nested clips nest properly.
    const float x0 = std::max(clip.x, previous.x);
    const float y0 = std::max(clip.y, previous.y);
    const float x1 = std::min(clip.x + clip.width, previous.x + previous.width);
    const float y1 = std::min(clip.y + clip.height, previous.y + previous.height);
    canvas.set_clip(Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)});
  }
  for (const layout::Box& child : box.children) paint_box(canvas, child, font, options, offset);
  paint_positioned(canvas, box, font, options, offset);
  canvas.set_clip(previous);
}

void paint_box(Canvas& canvas, const layout::Box& box, const text::Font& font, const RenderOptions& options,
               const Offset& offset_in) {
  // `position: fixed` is anchored to the viewport, so the scroll offset must not
  // move it (the whole subtree is painted with a zero offset).
  const Offset offset = box.fixed ? Offset{0.0f, 0.0f} : offset_in;

  switch (box.kind) {
    case layout::BoxKind::Block: {
      if (box.style != nullptr) {
        if (box.style->background_color.a > 0) {
          canvas.fill_rect(to_paint_rect(box.border_box, offset), box.style->background_color);
        }
        paint_border(canvas, box, offset);
        paint_marker(canvas, box, font, offset);
        if (options.debug_boxes) {
          canvas.stroke_rect(to_paint_rect(box.border_box, offset), css::Color{255, 0, 200, 90}, 1.0f);
        }
      }
      paint_children(canvas, box, font, options, offset);
      break;
    }
    case layout::BoxKind::Text: {
      if (box.style == nullptr) return;
      const float baseline = box.border_box.y + offset.y + box.baseline;
      const float pen =
          canvas.draw_text(font, box.text, box.border_box.x + offset.x, baseline, box.font_size, box.style->color);
      if (box.style->text_decoration == css::TextDecoration::Underline) {
        const float thickness = std::max(1.0f, box.font_size / 14.0f);
        canvas.fill_rect(Rect{box.border_box.x + offset.x, baseline + box.font_size * 0.12f,
                              pen - box.border_box.x - offset.x, thickness},
                         box.style->color);
      }
      return;
    }
    case layout::BoxKind::Image: {
      const Rect rect = to_paint_rect(box.border_box, offset);
      if (box.image != nullptr && !box.image->empty() && rect.width > 0.5f && rect.height > 0.5f) {
        // Nearest-neighbour scaling: fast, deterministic, and honest about the
        // missing resampling filters (stage 6 work).
        const int target_width = static_cast<int>(rect.width + 0.5f);
        const int target_height = static_cast<int>(rect.height + 0.5f);
        for (int y = 0; y < target_height; ++y) {
          const int source_y = static_cast<int>(static_cast<float>(y) * static_cast<float>(box.image->height) /
                                                static_cast<float>(target_height));
          for (int x = 0; x < target_width; ++x) {
            const int source_x = static_cast<int>(static_cast<float>(x) * static_cast<float>(box.image->width) /
                                                  static_cast<float>(target_width));
            const css::Color pixel{box.image->sample(source_x, source_y, 0), box.image->sample(source_x, source_y, 1),
                                   box.image->sample(source_x, source_y, 2),
                                   box.image->sample(source_x, source_y, 3)};
            canvas.blend_pixel(static_cast<int>(rect.x) + x, static_cast<int>(rect.y) + y, pixel);
          }
        }
        return;
      }
      // No decoder input for this source: a grey placeholder that keeps the layout honest.
      canvas.fill_rect(rect, css::Color{226, 226, 226, 255});
      canvas.stroke_rect(rect, css::Color{150, 150, 150, 255}, 1.0f);
      return;
    }
    case layout::BoxKind::InlineBox: {
      if (box.style != nullptr && box.style->background_color.a > 0) {
        canvas.fill_rect(to_paint_rect(box.border_box, offset), box.style->background_color);
      }
      paint_children(canvas, box, font, options, offset);
      return;
    }
  }
}

void paint_positioned(Canvas& canvas, const layout::Box& box, const text::Font& font, const RenderOptions& options,
                      const Offset& offset) {
  if (box.positioned.empty()) return;
  std::vector<const layout::Box*> stacked;
  stacked.reserve(box.positioned.size());
  for (const layout::Box& child : box.positioned) stacked.push_back(&child);
  // Stable: equal z-index keeps document order, which is what authors expect for
  // ordinary pages that never touch z-index at all.
  std::stable_sort(stacked.begin(), stacked.end(),
                   [](const layout::Box* a, const layout::Box* b) { return a->z_index < b->z_index; });
  for (const layout::Box* child : stacked) paint_box(canvas, *child, font, options, offset);
}

}  // namespace

void render_layout(Canvas& canvas, const layout::Box& root, const text::Font& font, const RenderOptions& options) {
  canvas.clear(options.page_background);
  paint_box(canvas, root, font, options, Offset{0.0f, -options.scroll_y});
}

}  // namespace bonk::paint
