#include <cstring>
#include <fstream>

#include "bonk/image/image.hpp"

namespace bonk::image {
namespace {

std::uint32_t read_u32(std::string_view data, std::size_t offset) {
  if (offset + 3 >= data.size()) return 0;
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) << 24) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3]));
}

Error png_error(std::string_view message) { return Error::make("image.png", message); }

int channels_for(std::uint8_t color_type) {
  switch (color_type) {
    case 0:
      return 1;  // grey
    case 2:
      return 3;  // rgb
    case 3:
      return 1;  // palette index
    case 4:
      return 2;  // grey + alpha
    case 6:
      return 4;  // rgba
    default:
      return 0;
  }
}

struct Header {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bit_depth = 8;
  std::uint8_t color_type = 6;
  std::uint8_t interlace = 0;
};

/// Undoes the five PNG scanline filters in place.
bool unfilter(std::string& raw, std::size_t offset, std::size_t row_bytes, std::size_t rows, std::size_t bpp) {
  std::vector<std::uint8_t> previous(row_bytes, 0);
  std::vector<std::uint8_t> current(row_bytes, 0);
  for (std::size_t y = 0; y < rows; ++y) {
    if (offset >= raw.size()) return false;
    const auto filter = static_cast<std::uint8_t>(raw[offset]);
    ++offset;
    if (offset + row_bytes > raw.size()) return false;
    std::memcpy(current.data(), raw.data() + offset, row_bytes);

    for (std::size_t i = 0; i < row_bytes; ++i) {
      const int a = i >= bpp ? current[i - bpp] : 0;
      const int b = previous[i];
      const int c = i >= bpp ? previous[i - bpp] : 0;
      int value = current[i];
      switch (filter) {
        case 0:
          break;
        case 1:
          value = (value + a) & 0xFF;
          break;
        case 2:
          value = (value + b) & 0xFF;
          break;
        case 3:
          value = (value + ((a + b) >> 1)) & 0xFF;
          break;
        case 4: {
          const int p = a + b - c;
          const int pa = std::abs(p - a);
          const int pb = std::abs(p - b);
          const int pc = std::abs(p - c);
          const int predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          value = (value + predictor) & 0xFF;
          break;
        }
        default:
          return false;  // unknown filter type
      }
      current[i] = static_cast<std::uint8_t>(value);
    }
    std::memcpy(raw.data() + offset, current.data(), row_bytes);
    offset += row_bytes;
    std::swap(previous, current);
  }
  return true;
}

struct Palette {
  std::vector<std::uint8_t> rgb;  // 3 bytes per entry
};

/// Expands one (possibly interlaced) pass into RGBA pixels.
void expand_pass(const std::string& raw, std::size_t row_bytes, std::uint32_t pass_width, std::uint32_t pass_height,
                 const Header& header, const Palette& palette, Image& image, std::uint32_t x_start,
                 std::uint32_t y_start, std::uint32_t x_step, std::uint32_t y_step) {
  const int channels = channels_for(header.color_type);
  const int depth = header.bit_depth;
  const std::size_t bits_per_pixel = static_cast<std::size_t>(channels) * static_cast<std::size_t>(depth);
  const std::size_t filter_bpp = bits_per_pixel / 8 == 0 ? 1 : bits_per_pixel / 8;

  std::size_t offset = 0;
  for (std::uint32_t y = 0; y < pass_height; ++y) {
    const std::uint8_t filter = static_cast<std::uint8_t>(raw[offset]);
    (void)filter;  // already undone by unfilter()
    offset += 1;
    const std::uint8_t* row = reinterpret_cast<const std::uint8_t*>(raw.data()) + offset;
    offset += row_bytes;

    for (std::uint32_t x = 0; x < pass_width; ++x) {
      auto sample = [&](std::size_t index) -> int {
        if (depth == 8) return row[index];
        if (depth == 16) return row[index * 2];  // high byte: we render 8-bit
        // Sub-byte depths: 1/2/4 bits per sample, MSB first.
        const std::size_t bit_offset = index * static_cast<std::size_t>(depth);
        const std::uint8_t byte = row[bit_offset / 8];
        const int shift = 8 - depth - static_cast<int>(bit_offset % 8);
        const int mask = (1 << depth) - 1;
        return (byte >> shift) & mask;
      };

      std::uint8_t r = 0;
      std::uint8_t g = 0;
      std::uint8_t b = 0;
      std::uint8_t a = 255;
      const std::size_t base = static_cast<std::size_t>(x) * static_cast<std::size_t>(channels);

      switch (header.color_type) {
        case 0: {
          const int value = sample(base);
          const int scaled = depth >= 8 ? value : value * 255 / ((1 << depth) - 1);
          r = g = b = static_cast<std::uint8_t>(scaled);
          break;
        }
        case 2:
          r = static_cast<std::uint8_t>(sample(base + 0));
          g = static_cast<std::uint8_t>(sample(base + 1));
          b = static_cast<std::uint8_t>(sample(base + 2));
          break;
        case 3: {
          const std::size_t index = static_cast<std::size_t>(sample(base));
          if (index * 3 + 2 < palette.rgb.size()) {
            r = palette.rgb[index * 3 + 0];
            g = palette.rgb[index * 3 + 1];
            b = palette.rgb[index * 3 + 2];
          }
          break;
        }
        case 4: {
          const int grey = sample(base + 0);
          const int alpha = sample(base + 1);
          r = g = b = static_cast<std::uint8_t>(depth >= 8 ? grey : grey * 255 / ((1 << depth) - 1));
          a = static_cast<std::uint8_t>(depth >= 8 ? alpha : alpha * 255 / ((1 << depth) - 1));
          break;
        }
        case 6:
          r = static_cast<std::uint8_t>(sample(base + 0));
          g = static_cast<std::uint8_t>(sample(base + 1));
          b = static_cast<std::uint8_t>(sample(base + 2));
          a = static_cast<std::uint8_t>(sample(base + 3));
          break;
        default:
          break;
      }

      const std::uint32_t image_x = x_start + x * x_step;
      const std::uint32_t image_y = y_start + y * y_step;
      if (image_x >= static_cast<std::uint32_t>(image.width) || image_y >= static_cast<std::uint32_t>(image.height)) {
        continue;
      }
      const std::size_t target =
          (static_cast<std::size_t>(image_y) * static_cast<std::size_t>(image.width) + image_x) * 4;
      image.rgba[target + 0] = r;
      image.rgba[target + 1] = g;
      image.rgba[target + 2] = b;
      image.rgba[target + 3] = a;
    }
  }
  (void)filter_bpp;
}

// Adam7 pass geometry: x_start, y_start, x_step, y_step.
constexpr std::uint32_t kAdam7[7][4] = {
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2},
};

}  // namespace

std::uint8_t Image::sample(int x, int y, int channel) const {
  if (x < 0 || y < 0 || x >= width || y >= height || channel < 0 || channel > 3) return 0;
  return rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4 +
              static_cast<std::size_t>(channel)];
}

Result<Image> decode_png(std::string_view bytes) {
  static const char kSignature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
  if (bytes.size() < 8 || std::memcmp(bytes.data(), kSignature, 8) != 0) {
    return Result<Image>::failure(png_error("not a PNG file (bad signature)"));
  }

  Header header;
  bool header_seen = false;
  Palette palette;
  std::string compressed;

  std::size_t position = 8;
  while (position + 8 <= bytes.size()) {
    const std::uint32_t length = read_u32(bytes, position);
    const std::string_view type = bytes.substr(position + 4, 4);
    const std::size_t data_start = position + 8;
    if (data_start + length > bytes.size()) return Result<Image>::failure(png_error("chunk runs past the file"));
    const std::string_view payload = bytes.substr(data_start, length);

    if (type == "IHDR") {
      if (length < 13) return Result<Image>::failure(png_error("truncated IHDR"));
      header.width = read_u32(payload, 0);
      header.height = read_u32(payload, 4);
      header.bit_depth = static_cast<std::uint8_t>(payload[8]);
      header.color_type = static_cast<std::uint8_t>(payload[9]);
      header.interlace = static_cast<std::uint8_t>(payload[12]);
      header_seen = true;
      if (header.width == 0 || header.height == 0) {
        return Result<Image>::failure(png_error("image has zero width or height"));
      }
      if (header.width > 16384 || header.height > 16384) {
        return Result<Image>::failure(png_error("image is larger than 16384x16384"));
      }
      if (channels_for(header.color_type) == 0) {
        return Result<Image>::failure(png_error("unsupported colour type"));
      }
      if (header.bit_depth != 1 && header.bit_depth != 2 && header.bit_depth != 4 && header.bit_depth != 8 &&
          header.bit_depth != 16) {
        return Result<Image>::failure(png_error("unsupported bit depth"));
      }
      if (header.interlace > 1) return Result<Image>::failure(png_error("unsupported interlace method"));
    } else if (type == "PLTE") {
      palette.rgb.assign(payload.begin(), payload.end());
    } else if (type == "IDAT") {
      compressed.append(payload);
    } else if (type == "IEND") {
      break;
    }
    position = data_start + length + 4;  // skip CRC (we do not verify it)
  }

  if (!header_seen) return Result<Image>::failure(png_error("missing IHDR"));
  if (compressed.empty()) return Result<Image>::failure(png_error("no image data (IDAT)"));

  Result<std::string> raw = inflate_zlib(compressed);
  if (raw.is_error()) return Result<Image>::failure(raw.error());

  Image image;
  image.width = static_cast<int>(header.width);
  image.height = static_cast<int>(header.height);
  image.rgba.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4, 0);

  const int channels = channels_for(header.color_type);
  const std::size_t bits_per_pixel = static_cast<std::size_t>(channels) * static_cast<std::size_t>(header.bit_depth);
  const std::size_t filter_bpp = bits_per_pixel / 8 == 0 ? 1 : bits_per_pixel / 8;

  auto row_bytes_for = [&](std::uint32_t width) {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(width) * bits_per_pixel + 7) / 8);
  };

  if (header.interlace == 0) {
    const std::size_t row_bytes = row_bytes_for(header.width);
    if (!unfilter(raw.value(), 0, row_bytes, header.height, filter_bpp)) {
      return Result<Image>::failure(png_error("corrupt scanline data"));
    }
    expand_pass(raw.value(), row_bytes, header.width, header.height, header, palette, image, 0, 0, 1, 1);
    return Result<Image>::success(std::move(image));
  }

  // Adam7: seven passes, each with its own geometry, concatenated in the stream.
  std::size_t offset = 0;
  for (const auto& pass : kAdam7) {
    const std::uint32_t x_start = pass[0];
    const std::uint32_t y_start = pass[1];
    const std::uint32_t x_step = pass[2];
    const std::uint32_t y_step = pass[3];
    if (x_start >= header.width || y_start >= header.height) continue;
    const std::uint32_t pass_width = (header.width - x_start + x_step - 1) / x_step;
    const std::uint32_t pass_height = (header.height - y_start + y_step - 1) / y_step;
    if (pass_width == 0 || pass_height == 0) continue;
    const std::size_t row_bytes = row_bytes_for(pass_width);
    const std::size_t needed = (row_bytes + 1) * pass_height;
    if (offset + needed > raw.value().size()) return Result<Image>::failure(png_error("truncated interlaced data"));
    std::string pass_data = raw.value().substr(offset, needed);
    if (!unfilter(pass_data, 0, row_bytes, pass_height, filter_bpp)) {
      return Result<Image>::failure(png_error("corrupt interlaced scanline"));
    }
    expand_pass(pass_data, row_bytes, pass_width, pass_height, header, palette, image, x_start, y_start, x_step,
                y_step);
    offset += needed;
  }
  return Result<Image>::success(std::move(image));
}

Result<Image> load_image(std::string_view path) {
  std::ifstream file{std::string(path), std::ios::binary};
  if (!file) return Result<Image>::failure(Error::make("image.open_failed", "cannot open '" + std::string(path) + "'"));
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() > 64u * 1024u * 1024u) {
    return Result<Image>::failure(Error::make("image.too_large", "image file is larger than 64 MiB"));
  }
  return decode_png(bytes);
}

bool ImageStore::add_from_file(std::string_view src, std::string_view base_directory, std::string& error) {
  std::string path(base_directory);
  if (!path.empty() && path.back() != '/') path += '/';
  path.append(src);
  Result<Image> image = load_image(path);
  if (image.is_error()) {
    error = image.error().to_string();
    return false;
  }
  images_.emplace(std::string(src), std::move(image.value()));
  return true;
}

void ImageStore::insert(std::string key, Image image) { images_[std::move(key)] = std::move(image); }

const Image* ImageStore::find(std::string_view src) const {
  const auto found = images_.find(std::string(src));
  return found == images_.end() ? nullptr : &found->second;
}

}  // namespace bonk::image
