// gzip (RFC 1952) and zlib (RFC 1950) content decoding on top of our own inflate.
#include <cstdint>

#include "bonk/image/image.hpp"
#include "bonk/net/http.hpp"

namespace bonk::net {
namespace {

std::uint32_t crc32(std::string_view data) {
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
      }
      table[i] = value;
    }
    ready = true;
  }
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : data) crc = table[(crc ^ static_cast<std::uint8_t>(byte)) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

Error gzip_error(std::string_view message) { return Error::make("net.gzip", message); }

}  // namespace

Result<std::string> decode_gzip(std::string_view body) {
  if (body.size() < 18) return Result<std::string>::failure(gzip_error("gzip stream is too short"));
  const auto first = static_cast<std::uint8_t>(body[0]);
  const auto second = static_cast<std::uint8_t>(body[1]);
  if (first != 0x1F || second != 0x8B) return Result<std::string>::failure(gzip_error("bad gzip magic"));
  if (static_cast<std::uint8_t>(body[2]) != 8) return Result<std::string>::failure(gzip_error("unsupported gzip method"));

  const auto flags = static_cast<std::uint8_t>(body[3]);
  std::size_t position = 10;  // magic, method, flags, mtime, xfl, os

  if ((flags & 0x04) != 0) {  // FEXTRA
    if (position + 2 > body.size()) return Result<std::string>::failure(gzip_error("truncated FEXTRA"));
    const std::size_t extra =
        static_cast<std::size_t>(static_cast<std::uint8_t>(body[position])) |
        (static_cast<std::size_t>(static_cast<std::uint8_t>(body[position + 1])) << 8);
    position += 2 + extra;
  }
  if ((flags & 0x08) != 0) {  // FNAME
    while (position < body.size() && body[position] != '\0') ++position;
    ++position;
  }
  if ((flags & 0x10) != 0) {  // FCOMMENT
    while (position < body.size() && body[position] != '\0') ++position;
    ++position;
  }
  if ((flags & 0x02) != 0) position += 2;  // FHCRC
  if (position + 8 > body.size()) return Result<std::string>::failure(gzip_error("truncated header"));

  const std::string_view deflate_data = body.substr(position, body.size() - position - 8);
  Result<std::string> inflated = image::inflate_raw(deflate_data);
  if (inflated.is_error()) return Result<std::string>::failure(gzip_error(inflated.error().message));

  const std::size_t tail = body.size() - 8;
  const auto stored_crc = static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail])) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 1])) << 8) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 2])) << 16) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 3])) << 24);
  if (crc32(inflated.value()) != stored_crc) {
    return Result<std::string>::failure(gzip_error("CRC-32 mismatch"));
  }
  const auto stored_size = static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 4])) |
                           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 5])) << 8) |
                           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 6])) << 16) |
                           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[tail + 7])) << 24);
  if (stored_size != static_cast<std::uint32_t>(inflated.value().size())) {
    return Result<std::string>::failure(gzip_error("ISIZE mismatch"));
  }
  return inflated;
}

Result<std::string> decode_zlib(std::string_view body) {
  Result<std::string> inflated = image::inflate_zlib(body);
  if (inflated.is_error()) return Result<std::string>::failure(gzip_error(inflated.error().message));
  return inflated;
}

}  // namespace bonk::net
