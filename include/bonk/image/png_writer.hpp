// Minimal PNG writer for captured still frames.
//
// Deliberately separate from image/png.cpp (the decoder, owned by QA): this
// module only encodes, uses stored (uncompressed) deflate blocks, and never
// touches decode paths. Kept out of paint/canvas.cpp for the same reason:
// writing a captured Android frame must not depend on the browser rasterizer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bonk::image {

/// Encodes straight-alpha RGBA8 pixels as PNG.
/// Returns an empty string when width/height are not positive or the buffer
/// size does not equal width*height*4, so callers can treat empty as failure.
[[nodiscard]] std::string encode_png_rgba(int width, int height, const std::vector<std::uint8_t>& rgba);

}  // namespace bonk::image
