#include <array>
#include <cstdint>

#include "bonk/image/image.hpp"

namespace bonk::image {
namespace {

class BitReader {
 public:
  explicit BitReader(std::string_view data) : data_(data) {}

  /// Reads `count` bits, least-significant bit first (deflate bit order).
  std::uint32_t bits(int count) {
    while (bit_count_ < count) {
      if (position_ >= data_.size()) {
        overrun_ = true;
        return 0;
      }
      buffer_ |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[position_])) << bit_count_;
      ++position_;
      bit_count_ += 8;
    }
    const std::uint32_t value = buffer_ & ((1u << count) - 1u);
    buffer_ >>= count;
    bit_count_ -= count;
    return value;
  }

  /// Restarts decoding at a new byte-aligned position (stored blocks).
  void reset(std::string_view data) {
    data_ = data;
    position_ = 0;
    buffer_ = 0;
    bit_count_ = 0;
    overrun_ = false;
  }

  void align_to_byte() {
    buffer_ = 0;
    bit_count_ = 0;
  }

  [[nodiscard]] bool overrun() const noexcept { return overrun_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }

 private:
  std::string_view data_;
  std::size_t position_ = 0;
  std::uint32_t buffer_ = 0;
  int bit_count_ = 0;
  bool overrun_ = false;
};

/// Canonical Huffman table: for each bit length, the codes of that length.
struct HuffmanTable {
  std::array<std::uint16_t, 16> counts{};
  std::array<std::uint16_t, 288> symbols{};

  bool build(const std::uint8_t* lengths, int n) {
    counts.fill(0);
    symbols.fill(0);
    for (int i = 0; i < n; ++i) counts[lengths[i]]++;
    if (counts[0] == n) return true;  // no codes: valid but empty

    // Check for an over-subscribed code space.
    int left = 1;
    for (int length = 1; length < 16; ++length) {
      left <<= 1;
      left -= counts[length];
      if (left < 0) return false;
    }

    std::array<std::uint16_t, 16> offsets{};
    offsets[1] = 0;
    for (int length = 1; length < 15; ++length) offsets[length + 1] = offsets[length] + counts[length];
    for (int i = 0; i < n; ++i) {
      if (lengths[i] != 0) symbols[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
    return true;
  }

  int decode(BitReader& reader) const {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length < 16; ++length) {
      code |= static_cast<int>(reader.bits(1));
      const int count = counts[length];
      if (code - first < count) return symbols[index + (code - first)];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    return -1;  // invalid code
  }
};

Error inflate_error(std::string_view message) { return Error::make("image.inflate", message); }

}  // namespace

Result<std::string> inflate_raw(std::string_view deflate_stream) {
  static const std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const std::uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                                  33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const std::uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

  BitReader reader(deflate_stream);
  std::string out;
  out.reserve(deflate_stream.size() * 2);

  for (;;) {
    const std::uint32_t final_block = reader.bits(1);
    const std::uint32_t block_type = reader.bits(2);
    if (reader.overrun()) return Result<std::string>::failure(inflate_error("unexpected end of stream"));

    if (block_type == 0) {  // stored
      reader.align_to_byte();
      const std::size_t start = reader.position();
      if (start + 4 > deflate_stream.size()) {
        return Result<std::string>::failure(inflate_error("truncated stored block"));
      }
      const auto length = static_cast<std::size_t>(static_cast<std::uint8_t>(deflate_stream[start])) |
                          (static_cast<std::size_t>(static_cast<std::uint8_t>(deflate_stream[start + 1])) << 8);
      const auto complement = static_cast<std::size_t>(static_cast<std::uint8_t>(deflate_stream[start + 2])) |
                              (static_cast<std::size_t>(static_cast<std::uint8_t>(deflate_stream[start + 3])) << 8);
      if ((length ^ 0xFFFFu) != complement) {
        return Result<std::string>::failure(inflate_error("stored block length check failed"));
      }
      // The reader over-read up to a byte when filling its bit buffer; recompute
      // the true offset from the byte-aligned position it reports.
      const std::size_t data_start = start + 4;
      if (data_start + length > deflate_stream.size()) {
        return Result<std::string>::failure(inflate_error("stored block runs past the input"));
      }
      out.append(deflate_stream.substr(data_start, length));
      reader.reset(deflate_stream.substr(data_start + length));
    } else if (block_type == 1 || block_type == 2) {
      HuffmanTable literal_codes;
      HuffmanTable distance_codes;

      if (block_type == 1) {  // fixed Huffman
        std::uint8_t lengths[288];
        for (int i = 0; i < 144; ++i) lengths[i] = 8;
        for (int i = 144; i < 256; ++i) lengths[i] = 9;
        for (int i = 256; i < 280; ++i) lengths[i] = 7;
        for (int i = 280; i < 288; ++i) lengths[i] = 8;
        literal_codes.build(lengths, 288);
        std::uint8_t distance_lengths[30];
        for (int i = 0; i < 30; ++i) distance_lengths[i] = 5;
        distance_codes.build(distance_lengths, 30);
      } else {  // dynamic Huffman
        static const std::uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,  11, 4, 12,
                                                          3,  13, 2,  14, 1, 15};
        const int literal_count = static_cast<int>(reader.bits(5)) + 257;
        const int distance_count = static_cast<int>(reader.bits(5)) + 1;
        const int code_length_count = static_cast<int>(reader.bits(4)) + 4;

        std::uint8_t code_lengths[19] = {0};
        for (int i = 0; i < code_length_count; ++i) {
          code_lengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(reader.bits(3));
        }
        HuffmanTable code_length_codes;
        if (!code_length_codes.build(code_lengths, 19)) {
          return Result<std::string>::failure(inflate_error("bad code-length table"));
        }

        std::uint8_t lengths[288 + 30] = {0};
        int index = 0;
        while (index < literal_count + distance_count) {
          const int symbol = code_length_codes.decode(reader);
          if (symbol < 0) return Result<std::string>::failure(inflate_error("bad code-length symbol"));
          if (symbol < 16) {
            lengths[index++] = static_cast<std::uint8_t>(symbol);
          } else if (symbol == 16) {
            if (index == 0) return Result<std::string>::failure(inflate_error("repeat with no previous length"));
            const std::uint8_t previous = lengths[index - 1];
            const int repeat = 3 + static_cast<int>(reader.bits(2));
            for (int i = 0; i < repeat && index < literal_count + distance_count; ++i) lengths[index++] = previous;
          } else if (symbol == 17) {
            const int repeat = 3 + static_cast<int>(reader.bits(3));
            index += repeat;
          } else {
            const int repeat = 11 + static_cast<int>(reader.bits(7));
            index += repeat;
          }
        }
        if (!literal_codes.build(lengths, literal_count)) {
          return Result<std::string>::failure(inflate_error("bad literal/length table"));
        }
        if (!distance_codes.build(lengths + literal_count, distance_count)) {
          return Result<std::string>::failure(inflate_error("bad distance table"));
        }
      }

      for (;;) {
        const int symbol = literal_codes.decode(reader);
        if (symbol < 0) return Result<std::string>::failure(inflate_error("bad literal symbol"));
        if (symbol < 256) {
          out.push_back(static_cast<char>(symbol));
          continue;
        }
        if (symbol == 256) break;  // end of block

        const int length_index = symbol - 257;
        if (length_index >= 29) return Result<std::string>::failure(inflate_error("bad length symbol"));
        const std::size_t length = kLengthBase[length_index] +
                                   (kLengthExtra[length_index] > 0
                                        ? reader.bits(kLengthExtra[length_index])
                                        : 0);
        const int distance_symbol = distance_codes.decode(reader);
        if (distance_symbol < 0 || distance_symbol >= 30) {
          return Result<std::string>::failure(inflate_error("bad distance symbol"));
        }
        const std::size_t distance = kDistanceBase[distance_symbol] +
                                     (kDistanceExtra[distance_symbol] > 0
                                          ? reader.bits(kDistanceExtra[distance_symbol])
                                          : 0);
        if (distance > out.size()) {
          return Result<std::string>::failure(inflate_error("distance exceeds output"));
        }
        const std::size_t start = out.size() - distance;
        for (std::size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
        if (out.size() > 256u * 1024u * 1024u) {
          return Result<std::string>::failure(inflate_error("output exceeds 256 MiB"));
        }
      }
    } else {
      return Result<std::string>::failure(inflate_error("invalid deflate block type 3"));
    }

    if (reader.overrun()) return Result<std::string>::failure(inflate_error("truncated stream"));
    if (final_block != 0) break;
  }

  return Result<std::string>::success(std::move(out));
}

Result<std::string> inflate_zlib(std::string_view zlib_stream) {
  if (zlib_stream.size() < 6) return Result<std::string>::failure(Error::make("image.zlib", "stream is too small"));
  const auto cmf = static_cast<std::uint8_t>(zlib_stream[0]);
  const auto flg = static_cast<std::uint8_t>(zlib_stream[1]);
  if ((cmf & 0x0F) != 8) return Result<std::string>::failure(Error::make("image.zlib", "unsupported compression method"));
  if (((cmf << 8) | flg) % 31 != 0) {
    return Result<std::string>::failure(Error::make("image.zlib", "header checksum mismatch"));
  }
  if ((flg & 0x20) != 0) {
    return Result<std::string>::failure(Error::make("image.zlib", "preset dictionaries are not supported"));
  }

  Result<std::string> inflated = inflate_raw(zlib_stream.substr(2, zlib_stream.size() - 6));
  if (inflated.is_error()) return inflated;

  // Adler-32 of the uncompressed data, stored big-endian at the end.
  const std::string& data = inflated.value();
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const char byte : data) {
    a = (a + static_cast<std::uint8_t>(byte)) % 65521u;
    b = (b + a) % 65521u;
  }
  const std::uint32_t expected = (b << 16) | a;
  const std::size_t tail = zlib_stream.size() - 4;
  const std::uint32_t stored = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(zlib_stream[tail])) << 24) |
                               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(zlib_stream[tail + 1])) << 16) |
                               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(zlib_stream[tail + 2])) << 8) |
                               static_cast<std::uint32_t>(static_cast<std::uint8_t>(zlib_stream[tail + 3]));
  if (stored != expected) {
    return Result<std::string>::failure(Error::make("image.zlib", "Adler-32 mismatch"));
  }
  return inflated;
}

}  // namespace bonk::image
