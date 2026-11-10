#include "bonk/image/png_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bonk::image {
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
  std::string crc_input(type);
  crc_input.append(payload);
  out.append(type);
  out.append(payload);
  append_u32(out, crc32_of(crc_input));
}

/// zlib stream made of stored deflate blocks: valid output without entropy
/// coding, matching the project's existing PNG writer policy.
std::string zlib_stored(std::string_view raw) {
  std::string out;
  out.push_back(static_cast<char>(0x78));
  out.push_back(static_cast<char>(0x01));

  constexpr std::size_t kMaxBlock = 65535;
  std::size_t offset = 0;
  do {
    const std::size_t block_size = std::min(kMaxBlock, raw.size() - offset);
    const bool final_block = offset + block_size == raw.size();
    out.push_back(static_cast<char>(final_block ? 1 : 0));
    out.push_back(static_cast<char>(block_size & 0xFF));
    out.push_back(static_cast<char>((block_size >> 8) & 0xFF));
    const std::uint16_t inverse = static_cast<std::uint16_t>(~block_size);
    out.push_back(static_cast<char>(inverse & 0xFF));
    out.push_back(static_cast<char>((inverse >> 8) & 0xFF));
    out.append(raw.substr(offset, block_size));
    offset += block_size;
  } while (offset < raw.size());

  append_u32(out, adler32_of(raw));
  return out;
}

}  // namespace

std::string encode_png_rgba(int width, int height, const std::vector<std::uint8_t>& rgba) {
  if (width <= 0 || height <= 0) return {};
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
  if (rgba.size() != row_bytes * static_cast<std::size_t>(height)) return {};

  // Raw scanlines: filter byte 0 followed by RGBA pixels.
  std::string raw;
  raw.reserve((row_bytes + 1) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.append(reinterpret_cast<const char*>(rgba.data()) + static_cast<std::size_t>(y) * row_bytes, row_bytes);
  }

  std::string png = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  append_u32(ihdr, static_cast<std::uint32_t>(width));
  append_u32(ihdr, static_cast<std::uint32_t>(height));
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // colour type: RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  append_chunk(png, "IHDR", ihdr);
  append_chunk(png, "IDAT", zlib_stored(raw));
  append_chunk(png, "IEND", {});
  return png;
}

}  // namespace bonk::image
