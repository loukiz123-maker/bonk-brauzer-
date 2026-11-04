#include "bonk/paint/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "bonk/core/utf8.hpp"

namespace bonk::paint {
namespace {

std::uint32_t crc32_of(std::string_view data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : data) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32_of(std::string_view data) {
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const char byte : data) {
    a = (a + static_cast<std::uint8_t>(byte)) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void append_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void append_chunk(std::string& out, std::string_view type, std::string_view payload) {
  append_u32(out, static_cast<std::uint32_t>(payload.size()));
  std::string crc_input;
  crc_input.append(type);
  crc_input.append(payload);
  out.append(type);
  out.append(payload);
  append_u32(out, crc32_of(crc_input));
}

/// zlib stream with stored deflate blocks (RFC 1951 §3.2.4).
std::string zlib_stored(std::string_view raw) {
  std::string out;
  out.push_back(0x78);  // CMF: deflate, 32K window
  out.push_back(0x01);  // FLG: no dictionary, fastest
  std::size_t position = 0;
  while (position < raw.size()) {
    const std::size_t chunk = std::min<std::size_t>(65535, raw.size() - position);
    const bool last = position + chunk >= raw.size();
    out.push_back(last ? 1 : 0);  // BFINAL, BTYPE=00 (stored)
    out.push_back(static_cast<char>(chunk & 0xFF));
    out.push_back(static_cast<char>((chunk >> 8) & 0xFF));
    out.push_back(static_cast<char>(~chunk & 0xFF));
    out.push_back(static_cast<char>((~chunk >> 8) & 0xFF));
    out.append(raw.substr(position, chunk));
    position += chunk;
  }
  append_u32(out, adler32_of(raw));
  return out;
}

std::uint8_t blend_channel(std::uint8_t destination, std::uint8_t source, float alpha) {
  const float blended = static_cast<float>(destination) * (1.0f - alpha) + static_cast<float>(source) * alpha;
  return static_cast<std::uint8_t>(std::clamp(blended + 0.5f, 0.0f, 255.0f));
}

}  // namespace

Canvas::Canvas(int width, int height)
    : width_(std::max(1, width)),
      height_(std::max(1, height)),
      pixels_(static_cast<std::size_t>(std::max(1, width)) * static_cast<std::size_t>(std::max(1, height)) * 4, 255) {}

void Canvas::clear(css::Color color) {
  for (std::size_t i = 0; i < pixels_.size(); i += 4) {
    pixels_[i + 0] = color.r;
    pixels_[i + 1] = color.g;
    pixels_[i + 2] = color.b;
    pixels_[i + 3] = color.a;
  }
}

void Canvas::blend_pixel(int x, int y, css::Color color) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
  if (color.a == 0) return;
  const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                             static_cast<std::size_t>(x)) * 4;
  if (color.a == 255) {
    pixels_[index + 0] = color.r;
    pixels_[index + 1] = color.g;
    pixels_[index + 2] = color.b;
    pixels_[index + 3] = 255;
    return;
  }
  const float alpha = static_cast<float>(color.a) / 255.0f;
  pixels_[index + 0] = blend_channel(pixels_[index + 0], color.r, alpha);
  pixels_[index + 1] = blend_channel(pixels_[index + 1], color.g, alpha);
  pixels_[index + 2] = blend_channel(pixels_[index + 2], color.b, alpha);
  pixels_[index + 3] = 255;
}

void Canvas::fill_rect(Rect rect, css::Color color) {
  if (color.a == 0 || rect.width <= 0.0f || rect.height <= 0.0f) return;
  const int x0 = std::max(0, static_cast<int>(std::floor(rect.x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(rect.y)));
  const int x1 = std::min(width_, static_cast<int>(std::ceil(rect.x + rect.width)));
  const int y1 = std::min(height_, static_cast<int>(std::ceil(rect.y + rect.height)));
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) blend_pixel(x, y, color);
  }
}

void Canvas::stroke_rect(Rect rect, css::Color color, float thickness) {
  if (thickness <= 0.0f) return;
  fill_rect(Rect{rect.x, rect.y, rect.width, thickness}, color);
  fill_rect(Rect{rect.x, rect.y + rect.height - thickness, rect.width, thickness}, color);
  fill_rect(Rect{rect.x, rect.y, thickness, rect.height}, color);
  fill_rect(Rect{rect.x + rect.width - thickness, rect.y, thickness, rect.height}, color);
}

void Canvas::draw_glyph(const text::Font& font, char32_t code_point, float x, float baseline_y, float font_size,
                        css::Color color) {
  if (code_point == U' ') return;
  const text::GlyphBitmap& bitmap = font.glyph(code_point, font_size);
  const int origin_x = static_cast<int>(std::lround(x)) + bitmap.left;
  const int origin_y = static_cast<int>(std::lround(baseline_y)) - bitmap.top;
  for (int gy = 0; gy < bitmap.height; ++gy) {
    for (int gx = 0; gx < bitmap.width; ++gx) {
      const std::uint8_t coverage = bitmap.at(gx, gy);
      if (coverage == 0) continue;
      css::Color tinted = color;
      tinted.a = static_cast<std::uint8_t>(static_cast<unsigned>(coverage) * color.a / 255u);
      blend_pixel(origin_x + gx, origin_y + gy, tinted);
    }
  }
}

float Canvas::draw_text(const text::Font& font, std::string_view text, float x, float baseline_y, float font_size,
                        css::Color color) {
  float pen = x;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const DecodedCodePoint decoded = decode_utf8(text, offset);
    draw_glyph(font, decoded.code_point, pen, baseline_y, font_size, color);
    pen += font.advance(decoded.code_point, font_size);
    offset += decoded.length;
  }
  return pen;
}

css::Color Canvas::pixel_at(int x, int y) const {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return css::Color{0, 0, 0, 0};
  const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                             static_cast<std::size_t>(x)) * 4;
  return css::Color{pixels_[index], pixels_[index + 1], pixels_[index + 2], pixels_[index + 3]};
}

bool Canvas::write_png(std::string_view path) const {
  // Raw scanlines: filter byte 0 followed by RGBA pixels.
  std::string raw;
  raw.reserve(static_cast<std::size_t>(height_) * (static_cast<std::size_t>(width_) * 4 + 1));
  for (int y = 0; y < height_; ++y) {
    raw.push_back(0);
    raw.append(reinterpret_cast<const char*>(pixels_.data()) + static_cast<std::size_t>(y) * width_ * 4,
               static_cast<std::size_t>(width_) * 4);
  }

  std::string png = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  append_u32(ihdr, static_cast<std::uint32_t>(width_));
  append_u32(ihdr, static_cast<std::uint32_t>(height_));
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // colour type: RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  append_chunk(png, "IHDR", ihdr);
  append_chunk(png, "IDAT", zlib_stored(raw));
  append_chunk(png, "IEND", {});

  std::ofstream file{std::string(path), std::ios::binary};
  if (!file) return false;
  file.write(png.data(), static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(file);
}

bool Canvas::write_ppm(std::string_view path) const {
  std::ofstream file{std::string(path), std::ios::binary};
  if (!file) return false;
  file << "P6\n" << width_ << ' ' << height_ << "\n255\n";
  std::string row;
  row.reserve(static_cast<std::size_t>(width_) * 3);
  for (int y = 0; y < height_; ++y) {
    row.clear();
    for (int x = 0; x < width_; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                                 static_cast<std::size_t>(x)) * 4;
      row.push_back(static_cast<char>(pixels_[index]));
      row.push_back(static_cast<char>(pixels_[index + 1]));
      row.push_back(static_cast<char>(pixels_[index + 2]));
    }
    file.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
  return static_cast<bool>(file);
}

}  // namespace bonk::paint
