// Bonk Brauzer — image decoding and a per-document image store.
//
// Implemented from scratch: RFC 1951 inflate (fixed + dynamic Huffman, no preset
// dictionary), the zlib wrapper (RFC 1950) and PNG (RFC 2083) for colour types
// 0/2/3/4/6 at bit depth 8 (plus 1/2/4/16 expansion for grey/PLTE), all five
// scanline filters and Adam7 interlacing.
// Not implemented yet: JPEG/GIF/WebP, PNG tRNS-free 16-bit precision (downscaled
// to 8), colour-space conversion (we pass samples through as sRGB).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk::image {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;  // width*height*4, straight alpha

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
  [[nodiscard]] std::uint8_t sample(int x, int y, int channel) const;
};

/// Raw deflate (no zlib header). Exposed because it is the interesting part.
[[nodiscard]] Result<std::string> inflate_raw(std::string_view deflate_stream);

/// zlib wrapper around inflate_raw: 2-byte header, optional dictionary, Adler-32.
[[nodiscard]] Result<std::string> inflate_zlib(std::string_view zlib_stream);

[[nodiscard]] Result<Image> decode_png(std::string_view bytes);

/// Loads and decodes an image by path (extension decides the codec; only PNG today).
[[nodiscard]] Result<Image> load_image(std::string_view path);

/// Images referenced by a page, keyed by the `src` string exactly as written in HTML.
class ImageStore {
 public:
  /// Loads `src` relative to `base_directory`; decode failures are recorded, not fatal.
  bool add_from_file(std::string_view src, std::string_view base_directory, std::string& error);
  void insert(std::string key, Image image);
  [[nodiscard]] const Image* find(std::string_view src) const;
  [[nodiscard]] std::size_t size() const noexcept { return images_.size(); }

 private:
  std::unordered_map<std::string, Image> images_;
};

}  // namespace bonk::image
