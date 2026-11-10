#include <cstdint>
#include <string>
#include <vector>

#include "bonk/image/image.hpp"
#include "bonk/image/png_writer.hpp"
#include "test_framework.hpp"

using namespace bonk;

BONK_TEST(png_writer, round_trips_through_the_real_decoder) {
  const int width = 3;
  const int height = 2;
  std::vector<std::uint8_t> rgba = {
      255, 0,   0,   255,  // opaque red
      0,   255, 0,   128,  // half-transparent green
      0,   0,   255, 0,    // fully transparent blue
      10,  20,  30,  255,  //
      40,  50,  60,  255,  //
      70,  80,  90,  255,  //
  };

  const std::string png = image::encode_png_rgba(width, height, rgba);
  REQUIRE(!png.empty());

  const Result<image::Image> decoded = image::decode_png(png);
  REQUIRE(decoded.is_ok());
  CHECK_EQ(decoded.value().width, width);
  CHECK_EQ(decoded.value().height, height);
  CHECK(decoded.value().rgba == rgba);
}

BONK_TEST(png_writer, round_trips_a_single_pixel) {
  std::vector<std::uint8_t> rgba = {1, 2, 3, 4};
  const std::string png = image::encode_png_rgba(1, 1, rgba);
  REQUIRE(!png.empty());
  const Result<image::Image> decoded = image::decode_png(png);
  REQUIRE(decoded.is_ok());
  CHECK(decoded.value().rgba == rgba);
}

BONK_TEST(png_writer, rejects_geometry_that_does_not_match_the_buffer) {
  std::vector<std::uint8_t> rgba = {1, 2, 3, 4};
  CHECK(image::encode_png_rgba(0, 1, rgba).empty());
  CHECK(image::encode_png_rgba(1, 0, rgba).empty());
  CHECK(image::encode_png_rgba(-1, 1, rgba).empty());
  // 2x2 needs 16 bytes, not 4.
  CHECK(image::encode_png_rgba(2, 2, rgba).empty());
  CHECK(image::encode_png_rgba(1, 1, {}).empty());
}

BONK_TEST(png_writer, output_starts_with_the_png_signature) {
  std::vector<std::uint8_t> rgba = {0, 0, 0, 255};
  const std::string png = image::encode_png_rgba(1, 1, rgba);
  REQUIRE(png.size() > 8);
  CHECK_EQ(png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
}

BONK_TEST(png_writer, large_rows_span_multiple_stored_blocks) {
  // 64x4 RGBA is 1024 bytes of scanlines; force a multi-block path with a
  // taller image where raw scanlines exceed one 65535-byte stored block.
  const int width = 64;
  const int height = 300;  // 64*300*4 + 300 = 77100 bytes of raw scanlines
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 0);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = static_cast<std::uint8_t>(i % 251);
    rgba[i + 3] = 255;
  }
  const std::string png = image::encode_png_rgba(width, height, rgba);
  REQUIRE(!png.empty());
  const Result<image::Image> decoded = image::decode_png(png);
  REQUIRE(decoded.is_ok());
  CHECK_EQ(decoded.value().width, width);
  CHECK_EQ(decoded.value().height, height);
  CHECK(decoded.value().rgba == rgba);
}

BONK_TEST(png_writer, multi_stored_block_streams_round_trip_exactly) {
  // Regression: a stored deflate block at the 65535-byte maximum, followed by
  // another block, used to lose its absolute position in the bit reader. The
  // second header was read from the first block's payload, so the decoder
  // reported "output exceeds the configured limit" or "truncated stored block"
  // instead of the real bytes. Sizes below/at/above the block boundary all
  // have to survive a full encode+decode round trip.
  for (const int height : {1, 2, 16, 205, 300, 512}) {
    const int width = 64;  // row = 257 bytes, so 255 rows cross 65535 bytes
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i] = static_cast<std::uint8_t>(i % 253);
      rgba[i + 1] = static_cast<std::uint8_t>((i / 7) % 251);
      rgba[i + 3] = 255;
    }
    const std::string png = image::encode_png_rgba(width, height, rgba);
    REQUIRE(!png.empty());
    const Result<image::Image> decoded = image::decode_png(png);
    REQUIRE(decoded.is_ok());
    CHECK_EQ(decoded.value().width, width);
    CHECK_EQ(decoded.value().height, height);
    CHECK(decoded.value().rgba == rgba);
  }
}

BONK_TEST(png_writer, exactly_one_full_stored_block_payload_still_decodes) {
  // Raw scanlines are height*(width*4+1). Pick sizes straddling the 65535-byte
  // stored-block boundary: the second block is where the regressed absolute
  // position handling used to appear.
  const int width = 64;  // row = 257 bytes
  const int height = 255;  // 255*257 = 65535 raw bytes exactly
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 9);
  const std::string png = image::encode_png_rgba(width, height, rgba);
  REQUIRE(!png.empty());
  const Result<image::Image> decoded = image::decode_png(png);
  REQUIRE(decoded.is_ok());
  CHECK(decoded.value().rgba == rgba);
}
