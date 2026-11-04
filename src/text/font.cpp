#include "bonk/text/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"

namespace bonk::text {
namespace {

struct Vec {
  float x = 0.0f;
  float y = 0.0f;
};

struct ContourPoint {
  Vec position;
  bool on_curve = false;
};

using Contour = std::vector<ContourPoint>;
using Polygon = std::vector<Vec>;

std::uint8_t read_u8(std::string_view data, std::size_t offset) {
  return offset < data.size() ? static_cast<std::uint8_t>(data[offset]) : 0;
}

std::uint16_t read_u16(std::string_view data, std::size_t offset) {
  if (offset + 1 >= data.size()) return 0;
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(data[offset]) << 8) |
                                    static_cast<std::uint8_t>(data[offset + 1]));
}

std::int16_t read_i16(std::string_view data, std::size_t offset) {
  return static_cast<std::int16_t>(read_u16(data, offset));
}

std::uint32_t read_u32(std::string_view data, std::size_t offset) {
  if (offset + 3 >= data.size()) return 0;
  return (static_cast<std::uint32_t>(read_u16(data, offset)) << 16) | read_u16(data, offset + 2);
}

std::string_view tag_at(std::string_view data, std::size_t offset) {
  if (offset + 4 > data.size()) return {};
  return data.substr(offset, 4);
}

/// Non-zero winding test for every polygon; used by the rasterizer.
bool inside_non_zero(const std::vector<Polygon>& polygons, Vec point) {
  int winding = 0;
  for (const Polygon& polygon : polygons) {
    const std::size_t count = polygon.size();
    for (std::size_t i = 0; i < count; ++i) {
      const Vec a = polygon[i];
      const Vec b = polygon[(i + 1) % count];
      if (a.y <= point.y) {
        if (b.y > point.y) {
          const float cross = (b.x - a.x) * (point.y - a.y) - (point.x - a.x) * (b.y - a.y);
          if (cross > 0.0f) ++winding;
        }
      } else if (b.y <= point.y) {
        const float cross = (b.x - a.x) * (point.y - a.y) - (point.x - a.x) * (b.y - a.y);
        if (cross < 0.0f) --winding;
      }
    }
  }
  return winding != 0;
}

void flatten_quadratic(Polygon& out, Vec p0, Vec control, Vec p1) {
  const float length = std::hypot(p1.x - p0.x, p1.y - p0.y) +
                       std::hypot(control.x - p0.x, control.y - p0.y) +
                       std::hypot(p1.x - control.x, p1.y - control.y);
  const int steps = std::clamp(static_cast<int>(length / 2.0f) + 2, 2, 24);
  for (int step = 1; step <= steps; ++step) {
    const float t = static_cast<float>(step) / static_cast<float>(steps);
    const float mt = 1.0f - t;
    out.push_back(Vec{mt * mt * p0.x + 2.0f * mt * t * control.x + t * t * p1.x,
                      mt * mt * p0.y + 2.0f * mt * t * control.y + t * t * p1.y});
  }
}

/// Converts TrueType contours (on/off curve points) into closed polygons.
std::vector<Polygon> contours_to_polygons(const std::vector<Contour>& contours) {
  std::vector<Polygon> polygons;
  for (const Contour& contour : contours) {
    if (contour.size() < 2) continue;

    Contour normalized;
    normalized.reserve(contour.size() + 1);
    // TrueType allows contours to start with an off-curve point: rotate so that
    // the first point is on-curve (inserting an implied midpoint if needed).
    if (!contour.front().on_curve) {
      if (contour.back().on_curve) {
        normalized.push_back(contour.back());
      } else {
        normalized.push_back(ContourPoint{
            Vec{(contour.front().position.x + contour.back().position.x) / 2.0f,
                (contour.front().position.y + contour.back().position.y) / 2.0f},
            true});
      }
    }
    for (const ContourPoint& point : contour) normalized.push_back(point);

    Polygon polygon;
    Vec current = normalized.front().position;
    polygon.push_back(current);
    std::size_t i = 1;
    while (i < normalized.size() + 1) {
      const ContourPoint& point = normalized[i % normalized.size()];
      if (point.on_curve) {
        current = point.position;
        polygon.push_back(current);
        ++i;
        continue;
      }
      // off-curve: collect implied on-curve midpoints until the next on-curve point
      Vec control = point.position;
      const ContourPoint& next = normalized[(i + 1) % normalized.size()];
      Vec end;
      if (next.on_curve) {
        end = next.position;
        i += 2;
      } else {
        end = Vec{(control.x + next.position.x) / 2.0f, (control.y + next.position.y) / 2.0f};
        i += 1;
      }
      flatten_quadratic(polygon, current, control, end);
      current = end;
    }
    if (polygon.size() >= 3) polygons.push_back(std::move(polygon));
  }
  return polygons;
}

}  // namespace

struct Font::Impl {
  std::string data;
  std::string font_name;
  bool fallback = true;

  std::uint32_t units_per_em = 1000;
  std::int16_t ascender = 800;
  std::int16_t descender = -200;
  std::int16_t line_gap = 100;
  std::uint16_t number_of_hmetrics = 0;
  std::uint16_t number_of_glyphs = 0;
  bool long_loca = false;

  std::size_t cmap_offset = 0;   // offset of the selected subtable
  std::uint16_t cmap_format = 4;
  std::size_t hmtx_offset = 0;
  std::size_t loca_offset = 0;
  std::size_t glyf_offset = 0;

  mutable std::unordered_map<std::uint64_t, GlyphBitmap> cache;

  [[nodiscard]] std::uint16_t glyph_index(char32_t code_point) const;
  [[nodiscard]] std::uint16_t advance_units(std::uint16_t glyph) const;
  [[nodiscard]] std::vector<Contour> load_glyph(std::uint16_t glyph, int depth) const;
  [[nodiscard]] GlyphBitmap rasterize(char32_t code_point, float pixel_size) const;
};

std::uint16_t Font::Impl::glyph_index(char32_t code_point) const {
  if (fallback || cmap_offset == 0) return 0;

  if (cmap_format == 12) {
    const std::uint32_t groups = read_u32(data, cmap_offset + 12);
    for (std::uint32_t i = 0; i < groups; ++i) {
      const std::size_t group = cmap_offset + 16 + static_cast<std::size_t>(i) * 12;
      const std::uint32_t start = read_u32(data, group);
      const std::uint32_t end = read_u32(data, group + 4);
      if (code_point >= start && code_point <= end) {
        return static_cast<std::uint16_t>(read_u32(data, group + 8) + (code_point - start));
      }
    }
    return 0;
  }

  // format 4
  const std::uint16_t segments = static_cast<std::uint16_t>(read_u16(data, cmap_offset + 6) / 2);
  const std::size_t end_codes = cmap_offset + 14;
  const std::size_t start_codes = end_codes + static_cast<std::size_t>(segments) * 2 + 2;
  const std::size_t id_deltas = start_codes + static_cast<std::size_t>(segments) * 2;
  const std::size_t id_range_offsets = id_deltas + static_cast<std::size_t>(segments) * 2;

  for (std::uint16_t i = 0; i < segments; ++i) {
    const std::uint16_t end = read_u16(data, end_codes + static_cast<std::size_t>(i) * 2);
    if (code_point > end) continue;
    const std::uint16_t start = read_u16(data, start_codes + static_cast<std::size_t>(i) * 2);
    if (code_point < start) return 0;
    const std::int16_t delta = static_cast<std::int16_t>(read_u16(data, id_deltas + static_cast<std::size_t>(i) * 2));
    const std::uint16_t range_offset = read_u16(data, id_range_offsets + static_cast<std::size_t>(i) * 2);
    if (range_offset == 0) {
      return static_cast<std::uint16_t>(static_cast<std::uint32_t>(code_point + delta) & 0xFFFF);
    }
    const std::size_t glyph_position = id_range_offsets + static_cast<std::size_t>(i) * 2 + range_offset +
                                       (code_point - start) * 2;
    const std::uint16_t glyph = read_u16(data, glyph_position);
    return glyph == 0 ? 0 : static_cast<std::uint16_t>(static_cast<std::uint32_t>(glyph + delta) & 0xFFFF);
  }
  return 0;
}

std::uint16_t Font::Impl::advance_units(std::uint16_t glyph) const {
  if (hmtx_offset == 0) return static_cast<std::uint16_t>(units_per_em / 2);
  const std::uint16_t index = number_of_hmetrics == 0
                                  ? 0
                                  : static_cast<std::uint16_t>(std::min<std::uint32_t>(glyph, number_of_hmetrics - 1));
  return read_u16(data, hmtx_offset + static_cast<std::size_t>(index) * 4);
}

std::vector<Contour> Font::Impl::load_glyph(std::uint16_t glyph, int depth) const {
  std::vector<Contour> contours;
  if (fallback || glyf_offset == 0 || glyph >= number_of_glyphs || depth > 4) return contours;

  const std::size_t loca_entry = long_loca ? 4 : 2;
  const std::size_t loca_position = loca_offset + static_cast<std::size_t>(glyph) * loca_entry;
  const std::uint32_t start =
      long_loca ? read_u32(data, loca_position) : static_cast<std::uint32_t>(read_u16(data, loca_position)) * 2;
  const std::uint32_t end = long_loca
                                ? read_u32(data, loca_position + 4)
                                : static_cast<std::uint32_t>(read_u16(data, loca_position + 2)) * 2;
  if (end <= start) return contours;  // empty glyph (space)

  const std::size_t base = glyf_offset + start;
  const std::int16_t number_of_contours = read_i16(data, base);

  if (number_of_contours < 0) {
    // Composite glyph: append components with their offsets/scales.
    std::size_t position = base + 10;
    for (;;) {
      const std::uint16_t flags = read_u16(data, position);
      const std::uint16_t component = read_u16(data, position + 2);
      position += 4;

      float dx = 0.0f;
      float dy = 0.0f;
      if ((flags & 0x0001) != 0) {  // ARG_1_AND_2_ARE_WORDS
        dx = read_i16(data, position);
        dy = read_i16(data, position + 2);
        position += 4;
      } else {
        dx = static_cast<std::int8_t>(read_u8(data, position));
        dy = static_cast<std::int8_t>(read_u8(data, position + 1));
        position += 2;
      }
      float scale = 1.0f;
      if ((flags & 0x0008) != 0) {  // WE_HAVE_A_SCALE
        scale = static_cast<float>(read_i16(data, position)) / 16384.0f;
        position += 2;
      } else if ((flags & 0x0040) != 0) {  // X_AND_Y_SCALE
        position += 4;
      } else if ((flags & 0x0080) != 0) {  // TWO_BY_TWO
        position += 8;
      }
      if ((flags & 0x0002) != 0) {  // ARGS_ARE_XY_VALUES
        // dx/dy already represent the offset
      } else {
        dx = 0.0f;
        dy = 0.0f;
      }

      for (Contour contour : load_glyph(component, depth + 1)) {
        for (ContourPoint& point : contour) {
          point.position.x = point.position.x * scale + dx;
          point.position.y = point.position.y * scale + dy;
        }
        contours.push_back(std::move(contour));
      }
      if ((flags & 0x0020) == 0) break;  // MORE_COMPONENTS
    }
    return contours;
  }

  if (number_of_contours == 0) return contours;

  const std::size_t end_points = base + 10;
  std::size_t position = end_points + static_cast<std::size_t>(number_of_contours) * 2;
  const std::uint16_t instruction_length = read_u16(data, position);
  position += 2 + instruction_length;

  std::vector<std::uint16_t> endpoints(static_cast<std::size_t>(number_of_contours));
  for (int i = 0; i < number_of_contours; ++i) {
    endpoints[static_cast<std::size_t>(i)] = read_u16(data, end_points + static_cast<std::size_t>(i) * 2);
  }
  const std::size_t point_count = static_cast<std::size_t>(endpoints.back()) + 1;

  std::vector<std::uint8_t> flags;
  flags.reserve(point_count);
  while (flags.size() < point_count) {
    const std::uint8_t flag = read_u8(data, position);
    ++position;
    flags.push_back(flag);
    if ((flag & 0x08) != 0) {  // REPEAT
      const std::uint8_t repeat = read_u8(data, position);
      ++position;
      for (std::uint8_t i = 0; i < repeat; ++i) flags.push_back(flag);
    }
  }

  std::vector<std::int32_t> xs(point_count, 0);
  std::vector<std::int32_t> ys(point_count, 0);
  std::int32_t value = 0;
  for (std::size_t i = 0; i < point_count; ++i) {
    const std::uint8_t flag = flags[i];
    if ((flag & 0x02) != 0) {  // X_SHORT_VECTOR
      const auto delta = static_cast<std::int32_t>(read_u8(data, position));
      ++position;
      value += (flag & 0x10) != 0 ? delta : -delta;
    } else if ((flag & 0x10) == 0) {
      value += read_i16(data, position);
      position += 2;
    }
    xs[i] = value;
  }
  value = 0;
  for (std::size_t i = 0; i < point_count; ++i) {
    const std::uint8_t flag = flags[i];
    if ((flag & 0x04) != 0) {  // Y_SHORT_VECTOR
      const auto delta = static_cast<std::int32_t>(read_u8(data, position));
      ++position;
      value += (flag & 0x20) != 0 ? delta : -delta;
    } else if ((flag & 0x20) == 0) {
      value += read_i16(data, position);
      position += 2;
    }
    ys[i] = value;
  }

  std::size_t point_index = 0;
  for (int i = 0; i < number_of_contours; ++i) {
    Contour contour;
    const std::uint16_t last_point = endpoints[static_cast<std::size_t>(i)];
    while (point_index <= last_point && point_index < point_count) {
      contour.push_back(ContourPoint{Vec{static_cast<float>(xs[point_index]), static_cast<float>(ys[point_index])},
                                     (flags[point_index] & 0x01) != 0});
      ++point_index;
    }
    contours.push_back(std::move(contour));
  }
  return contours;
}

GlyphBitmap Font::Impl::rasterize(char32_t code_point, float pixel_size) const {
  GlyphBitmap bitmap;
  const float size = std::max(1.0f, std::round(pixel_size));

  if (fallback) {
    if (code_point == U' ' || code_point == 0x00A0) return bitmap;  // pure advance, no ink
    // Tofu: an outlined box, so text still occupies realistic space.
    const int box_width = static_cast<int>(std::lround(size * 0.5f));
    const int box_height = static_cast<int>(std::lround(size * 0.7f));
    bitmap.width = std::max(2, box_width);
    bitmap.height = std::max(2, box_height);
    bitmap.left = 0;
    bitmap.top = box_height;
    bitmap.alpha.assign(static_cast<std::size_t>(bitmap.width * bitmap.height), 0);
    for (int y = 0; y < bitmap.height; ++y) {
      for (int x = 0; x < bitmap.width; ++x) {
        const bool edge = x == 0 || y == 0 || x == bitmap.width - 1 || y == bitmap.height - 1;
        bitmap.alpha[static_cast<std::size_t>(y * bitmap.width + x)] = edge ? 160 : 0;
      }
    }
    return bitmap;
  }

  const std::uint16_t glyph = glyph_index(code_point);
  if (glyph == 0) {
    // Missing glyph: draw a hollow "notdef" box so unsupported characters are
    // visible (a real browser shows the font's .notdef glyph; we synthesise one).
    const int box_width = static_cast<int>(std::lround(size * 0.5f));
    const int box_height = static_cast<int>(std::lround(size * 0.7f));
    bitmap.width = std::max(2, box_width);
    bitmap.height = std::max(2, box_height);
    bitmap.left = 0;
    bitmap.top = box_height;
    bitmap.alpha.assign(static_cast<std::size_t>(bitmap.width * bitmap.height), 0);
    for (int y = 0; y < bitmap.height; ++y) {
      for (int x = 0; x < bitmap.width; ++x) {
        const bool edge = x == 0 || y == 0 || x == bitmap.width - 1 || y == bitmap.height - 1;
        bitmap.alpha[static_cast<std::size_t>(y * bitmap.width + x)] = edge ? 120 : 0;
      }
    }
    return bitmap;
  }

  const std::vector<Contour> contours = load_glyph(glyph, 0);
  if (contours.empty()) return bitmap;
  const std::vector<Polygon> polygons = contours_to_polygons(contours);
  if (polygons.empty()) return bitmap;

  float x_min = 1e9f;
  float x_max = -1e9f;
  float y_min = 1e9f;
  float y_max = -1e9f;
  for (const Polygon& polygon : polygons) {
    for (const Vec& point : polygon) {
      x_min = std::min(x_min, point.x);
      x_max = std::max(x_max, point.x);
      y_min = std::min(y_min, point.y);
      y_max = std::max(y_max, point.y);
    }
  }

  const float scale = size / static_cast<float>(units_per_em);
  // Box in pixel space; y grows down in the bitmap.
  const int left = static_cast<int>(std::floor(x_min * scale)) - 1;
  const int right = static_cast<int>(std::ceil(x_max * scale)) + 1;
  const int top = static_cast<int>(std::ceil(y_max * scale)) + 1;
  const int bottom = static_cast<int>(std::floor(y_min * scale)) - 1;

  bitmap.width = std::max(1, right - left);
  bitmap.height = std::max(1, top - bottom);
  bitmap.left = left;
  bitmap.top = top;
  bitmap.alpha.assign(static_cast<std::size_t>(bitmap.width * bitmap.height), 0);

  constexpr int kSamples = 4;
  const float inverse_scale = 1.0f / scale;
  for (int y = 0; y < bitmap.height; ++y) {
    for (int x = 0; x < bitmap.width; ++x) {
      int covered = 0;
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          const float pixel_x = static_cast<float>(left + x) + (static_cast<float>(sx) + 0.5f) / kSamples;
          const float pixel_y = static_cast<float>(top - y - 1) + (static_cast<float>(sy) + 0.5f) / kSamples;
          const Vec point{pixel_x * inverse_scale, pixel_y * inverse_scale};
          if (inside_non_zero(polygons, point)) ++covered;
        }
      }
      if (covered > 0) {
        bitmap.alpha[static_cast<std::size_t>(y * bitmap.width + x)] =
            static_cast<std::uint8_t>(covered * 255 / (kSamples * kSamples));
      }
    }
  }
  return bitmap;
}

Font::Font() : impl_(std::make_shared<Impl>()) {}

Font::~Font() = default;
Font::Font(const Font&) = default;
Font& Font::operator=(const Font&) = default;
Font::Font(Font&&) noexcept = default;
Font& Font::operator=(Font&&) noexcept = default;

Result<Font> Font::load_memory(std::string_view bytes, std::string name) {
  if (bytes.size() < 12) return Result<Font>::failure(Error::make("font.truncated", "file is too small"));

  const std::uint32_t version = read_u32(bytes, 0);
  if (version != 0x00010000 && tag_at(bytes, 0) != "true" && tag_at(bytes, 0) != "OTTO") {
    return Result<Font>::failure(Error::make("font.unknown_format", "not a TrueType/OpenType file"));
  }
  if (tag_at(bytes, 0) == "OTTO") {
    return Result<Font>::failure(
        Error::make("font.cff_unsupported", "CFF-flavoured OpenType is not supported yet; use a TrueType font"));
  }

  auto impl = std::make_shared<Impl>();
  impl->data = std::string(bytes);
  impl->font_name = name.empty() ? std::string("<memory>") : std::move(name);
  impl->fallback = false;

  const std::uint16_t table_count = read_u16(impl->data, 4);
  std::size_t head = 0;
  std::size_t hhea = 0;
  std::size_t maxp = 0;
  std::size_t cmap = 0;
  std::size_t glyf = 0;
  std::size_t loca = 0;

  for (std::uint16_t i = 0; i < table_count; ++i) {
    const std::size_t record = 12 + static_cast<std::size_t>(i) * 16;
    const std::string_view tag = tag_at(impl->data, record);
    const std::uint32_t offset = read_u32(impl->data, record + 8);
    if (tag == "head") head = offset;
    if (tag == "hhea") hhea = offset;
    if (tag == "maxp") maxp = offset;
    if (tag == "cmap") cmap = offset;
    if (tag == "glyf") glyf = offset;
    if (tag == "loca") loca = offset;
    if (tag == "hmtx") impl->hmtx_offset = offset;
  }

  if (head == 0 || hhea == 0 || maxp == 0 || cmap == 0 || glyf == 0 || loca == 0) {
    return Result<Font>::failure(
        Error::make("font.missing_tables", "required TrueType tables (head/hhea/maxp/cmap/glyf/loca) are missing"));
  }

  impl->units_per_em = read_u16(impl->data, head + 18);
  if (impl->units_per_em == 0) impl->units_per_em = 1000;
  impl->long_loca = read_i16(impl->data, head + 50) == 1;
  impl->ascender = read_i16(impl->data, hhea + 4);
  impl->descender = read_i16(impl->data, hhea + 6);
  impl->line_gap = read_i16(impl->data, hhea + 8);
  impl->number_of_hmetrics = read_u16(impl->data, hhea + 34);
  impl->number_of_glyphs = read_u16(impl->data, maxp + 4);
  impl->loca_offset = loca;
  impl->glyf_offset = glyf;

  // Pick the best cmap subtable: prefer full Unicode (format 12), else BMP (format 4).
  const std::uint16_t cmap_tables = read_u16(impl->data, cmap + 2);
  std::size_t best_format4 = 0;
  std::size_t best_format12 = 0;
  for (std::uint16_t i = 0; i < cmap_tables; ++i) {
    const std::size_t record = cmap + 4 + static_cast<std::size_t>(i) * 8;
    const std::uint16_t platform = read_u16(impl->data, record);
    const std::uint16_t encoding = read_u16(impl->data, record + 2);
    const std::uint32_t offset = read_u32(impl->data, record + 4);
    const std::size_t subtable = cmap + offset;
    const std::uint16_t format = read_u16(impl->data, subtable);
    const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
    if (!unicode) continue;
    if (format == 12 && best_format12 == 0) best_format12 = subtable;
    if (format == 4 && best_format4 == 0) best_format4 = subtable;
  }
  if (best_format12 != 0) {
    impl->cmap_offset = best_format12;
    impl->cmap_format = 12;
  } else if (best_format4 != 0) {
    impl->cmap_offset = best_format4;
    impl->cmap_format = 4;
  } else {
    return Result<Font>::failure(Error::make("font.no_cmap", "no Unicode cmap subtable found"));
  }

  Font font;
  font.impl_ = std::move(impl);
  return Result<Font>::success(std::move(font));
}

Result<Font> Font::load_file(std::string_view path) {
  std::ifstream file{std::string(path), std::ios::binary};
  if (!file) {
    return Result<Font>::failure(Error::make("font.open_failed", "cannot open '" + std::string(path) + "'"));
  }
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return load_memory(bytes, std::string(path));
}

Font Font::system_default() {
  if (const char* from_env = std::getenv("BONK_FONT"); from_env != nullptr && *from_env != '\0') {
    if (Result<Font> font = load_file(from_env); font.is_ok()) return std::move(font.value());
  }
  static const char* const kCandidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "C:/Windows/Fonts/arial.ttf",
  };
  for (const char* path : kCandidates) {
    if (Result<Font> font = load_file(path); font.is_ok()) return std::move(font.value());
  }
  return Font{};
}

bool Font::is_fallback() const noexcept { return impl_ == nullptr || impl_->fallback; }

std::string_view Font::name() const noexcept {
  if (impl_ == nullptr) return "<none>";
  return impl_->fallback ? "<builtin-tofu>" : std::string_view(impl_->font_name);
}

float Font::ascent(float pixel_size) const {
  if (is_fallback()) return pixel_size * 0.75f;
  return static_cast<float>(impl_->ascender) * pixel_size / static_cast<float>(impl_->units_per_em);
}

float Font::descent(float pixel_size) const {
  if (is_fallback()) return pixel_size * 0.25f;
  return -static_cast<float>(impl_->descender) * pixel_size / static_cast<float>(impl_->units_per_em);
}

float Font::line_height(float pixel_size) const {
  if (is_fallback()) return pixel_size * 1.2f;
  const float units = static_cast<float>(impl_->ascender) - static_cast<float>(impl_->descender) +
                      static_cast<float>(impl_->line_gap);
  return units * pixel_size / static_cast<float>(impl_->units_per_em);
}

float Font::advance(char32_t code_point, float pixel_size) const {
  if (is_fallback()) return code_point == U' ' ? pixel_size * 0.3f : pixel_size * 0.55f;
  const std::uint16_t glyph = impl_->glyph_index(code_point);
  const float units = glyph == 0 ? static_cast<float>(impl_->units_per_em) * 0.55f
                                 : static_cast<float>(impl_->advance_units(glyph));
  return units * pixel_size / static_cast<float>(impl_->units_per_em);
}

float Font::space_width(float pixel_size) const { return advance(U' ', pixel_size); }

float Font::text_width(std::string_view utf8, float pixel_size) const {
  float total = 0.0f;
  std::size_t offset = 0;
  while (offset < utf8.size()) {
    const DecodedCodePoint decoded = decode_utf8(utf8, offset);
    total += advance(decoded.code_point, pixel_size);
    offset += decoded.length;
  }
  return total;
}

bool Font::has_glyph(char32_t code_point) const {
  if (is_fallback()) return code_point == U' ' || (code_point >= 0x20 && code_point < 0x7F);
  return impl_->glyph_index(code_point) != 0;
}

const GlyphBitmap& Font::glyph(char32_t code_point, float pixel_size) const {
  const float size = std::max(1.0f, std::round(pixel_size));
  const auto key = (static_cast<std::uint64_t>(code_point) << 12) |
                   (static_cast<std::uint64_t>(size) & 0xFFFu);
  auto& cache = impl_->cache;
  const auto cached = cache.find(key);
  if (cached != cache.end()) return cached->second;
  GlyphBitmap bitmap = impl_->rasterize(code_point, size);
  return cache.emplace(key, std::move(bitmap)).first->second;
}

}  // namespace bonk::text
