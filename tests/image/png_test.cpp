#include <cstdint>
#include <string>
#include <vector>

#include "bonk/image/image.hpp"
#include "test_framework.hpp"

using namespace bonk;

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

void append_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

std::string chunk(std::string_view type, std::string_view payload) {
  std::string out;
  append_u32(out, static_cast<std::uint32_t>(payload.size()));
  out.append(type);
  out.append(payload);
  append_u32(out, crc32_of(std::string(type) + std::string(payload)));
  return out;
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

/// Wraps raw deflate data in a zlib container with the correct Adler-32.
std::string zlib_wrap(std::string_view deflate_data, std::string_view uncompressed) {
  std::string out;
  out.push_back(static_cast<char>(0x78));
  out.push_back(static_cast<char>(0x9C));
  out.append(deflate_data);
  append_u32(out, adler32_of(uncompressed));
  return out;
}

/// Stored (uncompressed) deflate block — the simplest possible valid stream.
std::string stored_deflate(std::string_view data) {
  std::string out;
  out.push_back(static_cast<char>(0x01));  // BFINAL=1, BTYPE=00
  const auto length = static_cast<std::uint16_t>(data.size());
  out.push_back(static_cast<char>(length & 0xFF));
  out.push_back(static_cast<char>((length >> 8) & 0xFF));
  out.push_back(static_cast<char>(~length & 0xFF));
  out.push_back(static_cast<char>((~length >> 8) & 0xFF));
  out.append(data);
  return out;
}

struct PngBuilder {
  std::string header;
  std::string payload;
  std::string palette;

  PngBuilder(std::uint32_t width, std::uint32_t height, std::uint8_t bit_depth, std::uint8_t color_type,
             std::uint8_t interlace = 0) {
    header.push_back(static_cast<char>((width >> 24) & 0xFF));
    header.push_back(static_cast<char>((width >> 16) & 0xFF));
    header.push_back(static_cast<char>((width >> 8) & 0xFF));
    header.push_back(static_cast<char>(width & 0xFF));
    header.push_back(static_cast<char>((height >> 24) & 0xFF));
    header.push_back(static_cast<char>((height >> 16) & 0xFF));
    header.push_back(static_cast<char>((height >> 8) & 0xFF));
    header.push_back(static_cast<char>(height & 0xFF));
    header.push_back(static_cast<char>(bit_depth));
    header.push_back(static_cast<char>(color_type));
    header.push_back(0);
    header.push_back(0);
    header.push_back(static_cast<char>(interlace));
  }

  std::string build() const {
    std::string out("\x89PNG\r\n\x1a\n", 8);
    out += chunk("IHDR", header);
    if (!palette.empty()) out += chunk("PLTE", palette);
    out += chunk("IDAT", zlib_wrap(stored_deflate(payload), payload));
    out += chunk("IEND", "");
    return out;
  }
};

}  // namespace

BONK_TEST(inflate, stored_block_round_trip) {
  const std::string data = "hello, deflate";
  const Result<std::string> inflated = image::inflate_raw(stored_deflate(data));
  REQUIRE(inflated.is_ok());
  CHECK_EQ(inflated.value(), data);
}

BONK_TEST(inflate, zlib_wrapper_verifies_adler32) {
  const std::string data = "abcabcabcabc";
  std::string stream = zlib_wrap(stored_deflate(data), data);
  // NB: keep the Result alive — `Result::value()` returns a reference, so calling it
  // on a temporary inside a macro would dangle.
  const Result<std::string> inflated = image::inflate_zlib(stream);
  REQUIRE(inflated.is_ok());
  CHECK_EQ(inflated.value(), data);

  stream.back() = static_cast<char>(stream.back() ^ 0xFF);  // corrupt the checksum
  CHECK(image::inflate_zlib(stream).is_error());
}

BONK_TEST(inflate, rejects_garbage) {
  CHECK(image::inflate_raw(std::string("\x07\xff", 2)).is_error());
  CHECK(image::inflate_zlib("tiny").is_error());
}

BONK_TEST(inflate, enforces_output_limits_for_raw_and_zlib_streams) {
  const std::string data = "bounded output";
  const std::string deflate = stored_deflate(data);
  CHECK(image::inflate_raw(deflate, data.size() - 1).is_error());
  const Result<std::string> raw = image::inflate_raw(deflate, data.size());
  REQUIRE(raw.is_ok());
  CHECK_EQ(raw.value(), data);

  const std::string zlib = zlib_wrap(deflate, data);
  CHECK(image::inflate_zlib(zlib, data.size() - 1).is_error());
  const Result<std::string> wrapped = image::inflate_zlib(zlib, data.size());
  REQUIRE(wrapped.is_ok());
  CHECK_EQ(wrapped.value(), data);
}

BONK_TEST(png, decodes_rgb_with_sub_filter) {
  // 2x2 RGB: row 0 = red, green; row 1 uses the "Sub" filter.
  std::string raw;
  raw.push_back(0);
  raw.append("\xff\x00\x00\x00\xff\x00", 6);
  raw.push_back(1);
  for (int i = 0; i < 6; ++i) raw.push_back(static_cast<char>(i < 3 ? 0x10 : 0x00));

  PngBuilder builder(2, 2, 8, 2);
  builder.payload = raw;
  const Result<image::Image> decoded = image::decode_png(builder.build());
  REQUIRE(decoded.is_ok());
  const image::Image& decoded_image = decoded.value();
  CHECK_EQ(decoded_image.width, 2);
  CHECK_EQ(decoded_image.height, 2);
  CHECK_EQ(static_cast<int>(decoded_image.sample(0, 0, 0)), 255);
  CHECK_EQ(static_cast<int>(decoded_image.sample(1, 0, 1)), 255);
  // Sub filter: first pixel 0x10,0x10,0x10; second pixel adds 0 to each channel.
  CHECK_EQ(static_cast<int>(decoded_image.sample(0, 1, 2)), 0x10);
  CHECK_EQ(static_cast<int>(decoded_image.sample(1, 1, 1)), 0x10);
}

BONK_TEST(png, decodes_palette_and_alpha) {
  std::string raw;
  raw.push_back(0);
  raw.append("\x00\x01", 2);  // indices 0,1 → 0x01 0x01 0x01 0x01

  PngBuilder builder(2, 1, 8, 3);
  builder.palette.assign("\xff\x00\x00" "\x00\x00\xff", 6);
  builder.payload = raw;
  const Result<image::Image> decoded = image::decode_png(builder.build());
  REQUIRE(decoded.is_ok());
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 0)), 255);
  CHECK_EQ(static_cast<int>(decoded.value().sample(1, 0, 2)), 255);
  CHECK_EQ(static_cast<int>(decoded.value().sample(1, 0, 0)), 0);
  CHECK_EQ(static_cast<int>(decoded.value().sample(1, 0, 3)), 255);
}

BONK_TEST(png, decodes_a_minimal_adam7_image) {
  PngBuilder builder(1, 1, 8, 6, 1);
  builder.payload.assign(5, static_cast<char>(0));
  builder.payload[1] = static_cast<char>(0x12);
  builder.payload[2] = static_cast<char>(0x34);
  builder.payload[3] = static_cast<char>(0x56);
  builder.payload[4] = static_cast<char>(0xFF);
  const Result<image::Image> decoded = image::decode_png(builder.build());
  REQUIRE(decoded.is_ok());
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 0)), 0x12);
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 1)), 0x34);
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 2)), 0x56);
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 3)), 0xFF);
}

BONK_TEST(png, decodes_grayscale_alpha) {
  std::string raw;
  raw.push_back(0);
  raw.append("\x40\x80", 2);
  PngBuilder builder(1, 1, 8, 4);
  builder.payload = raw;
  const Result<image::Image> decoded = image::decode_png(builder.build());
  REQUIRE(decoded.is_ok());
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 0)), 0x40);
  CHECK_EQ(static_cast<int>(decoded.value().sample(0, 0, 3)), 0x80);
}

BONK_TEST(png, rejects_broken_input) {
  CHECK(image::decode_png("not a png at all").is_error());
  CHECK(image::decode_png(std::string("\x89PNG\r\n\x1a\n", 8)).is_error());  // signature only
}

BONK_TEST(png, enforces_pixel_limit_before_decoding) {
  PngBuilder builder(2, 1, 8, 2);
  builder.payload.assign(7, static_cast<char>(0));
  const std::string png = builder.build();
  CHECK(image::decode_png(png, 1).is_error());
  const Result<image::Image> decoded = image::decode_png(png, 2);
  REQUIRE(decoded.is_ok());
  CHECK_EQ(decoded.value().width, 2);
  CHECK_EQ(decoded.value().height, 1);
}

BONK_TEST(png, enforces_global_inflated_and_rgba_memory_limits) {
  const PngBuilder huge_inflated(8192, 8192, 16, 6);
  CHECK(image::decode_png(huge_inflated.build()).is_error());

  const PngBuilder huge_rgba(8193, 8192, 8, 0);
  CHECK(image::decode_png(huge_rgba.build()).is_error());
}

BONK_TEST(image_store, keeps_the_src_spelling) {
  image::ImageStore store;
  image::Image tiny;
  tiny.width = 1;
  tiny.height = 1;
  tiny.rgba.assign(4, 200);
  store.insert("sub/logo.PNG", tiny);
  REQUIRE(store.find("sub/logo.PNG") != nullptr);
  CHECK(store.find("sub/logo.png") == nullptr);
  CHECK_EQ(store.find("sub/logo.PNG")->sample(0, 0, 0), 200);
  std::string error;
  CHECK(!store.add_from_file("definitely-missing.png", "tests/fixtures", error));
  CHECK(!error.empty());
}
