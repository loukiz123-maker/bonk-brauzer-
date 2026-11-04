#include "bonk/crypto/crypto.hpp"

#include <cstring>

namespace bonk::crypto {
namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }

void sha256_compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choice + kSha256K[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

inline std::uint32_t load32_le(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void store32_le(std::uint8_t* p, std::uint32_t value) {
  p[0] = static_cast<std::uint8_t>(value);
  p[1] = static_cast<std::uint8_t>(value >> 8);
  p[2] = static_cast<std::uint8_t>(value >> 16);
  p[3] = static_cast<std::uint8_t>(value >> 24);
}

inline std::uint32_t rotl32(std::uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

inline void quarter_round(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d) {
  a += b;
  d ^= a;
  d = rotl32(d, 16);
  c += d;
  b ^= c;
  b = rotl32(b, 12);
  a += b;
  d ^= a;
  d = rotl32(d, 8);
  c += d;
  b ^= c;
  b = rotl32(b, 7);
}

}  // namespace

Sha256::Sha256() {
  state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
}

void Sha256::update(const void* data, std::size_t length) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total += length;
  while (length > 0) {
    const std::size_t take = std::min(length, block.size() - block_size);
    std::memcpy(block.data() + block_size, bytes, take);
    block_size += take;
    bytes += take;
    length -= take;
    if (block_size == block.size()) {
      sha256_compress(state, block.data());
      block_size = 0;
    }
  }
}

std::array<std::uint8_t, 32> Sha256::finish() {
  const std::uint64_t bit_length = total * 8;
  const std::uint8_t pad = 0x80;
  update(&pad, 1);
  const std::uint8_t zero = 0;
  while (block_size != 56) update(&zero, 1);
  std::uint8_t length_bytes[8];
  for (int i = 0; i < 8; ++i) length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
  update(length_bytes, 8);

  std::array<std::uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
    digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

std::array<std::uint8_t, 32> sha256(std::string_view data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::array<std::uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data) {
  std::array<std::uint8_t, 64> padded{};
  if (key.size() > 64) {
    const std::array<std::uint8_t, 32> hashed = sha256(key);
    std::memcpy(padded.data(), hashed.data(), hashed.size());
  } else {
    std::memcpy(padded.data(), key.data(), key.size());
  }

  std::array<std::uint8_t, 64> inner{};
  std::array<std::uint8_t, 64> outer{};
  for (std::size_t i = 0; i < 64; ++i) {
    inner[i] = static_cast<std::uint8_t>(padded[i] ^ 0x36);
    outer[i] = static_cast<std::uint8_t>(padded[i] ^ 0x5C);
  }

  Sha256 first;
  first.update(inner.data(), inner.size());
  first.update(data);
  const std::array<std::uint8_t, 32> inner_hash = first.finish();

  Sha256 second;
  second.update(outer.data(), outer.size());
  second.update(inner_hash.data(), inner_hash.size());
  return second.finish();
}

std::array<std::uint8_t, 32> hkdf_extract(std::string_view salt, std::string_view ikm) {
  return hmac_sha256(salt, ikm);
}

Bytes hkdf_expand(std::string_view prk, std::string_view info, std::size_t length) {
  Bytes output;
  output.reserve(length);
  std::array<std::uint8_t, 32> previous{};
  std::size_t previous_size = 0;
  std::uint8_t counter = 1;
  while (output.size() < length) {
    std::string block;
    block.reserve(previous_size + info.size() + 1);
    block.append(reinterpret_cast<const char*>(previous.data()), previous_size);
    block.append(info);
    block.push_back(static_cast<char>(counter));
    previous = hmac_sha256(prk, block);
    previous_size = previous.size();
    const std::size_t take = std::min(previous.size(), length - output.size());
    output.insert(output.end(), previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(take));
    ++counter;
  }
  return output;
}

Bytes hkdf_expand_label_with_context(std::string_view secret, std::string_view label, std::string_view context,
                                     std::size_t length) {
  std::string info;
  info.push_back(static_cast<char>((length >> 8) & 0xFF));
  info.push_back(static_cast<char>(length & 0xFF));
  info.push_back(static_cast<char>(6 + label.size()));  // "tls13 " + label
  info.append("tls13 ");
  info.append(label);
  info.push_back(static_cast<char>(context.size()));
  info.append(context);
  return hkdf_expand(secret, info, length);
}

Bytes hkdf_expand_label(std::string_view secret, std::string_view label, std::size_t length) {
  return hkdf_expand_label_with_context(secret, label, std::string_view{}, length);
}

void chacha20_block(const std::uint32_t key[8], std::uint32_t counter, const std::uint32_t nonce[3],
                    std::uint8_t out[64]) {
  std::uint32_t state[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                             key[0],     key[1],     key[2],     key[3],
                             key[4],     key[5],     key[6],     key[7],
                             counter,    nonce[0],   nonce[1],   nonce[2]};
  std::uint32_t working[16];
  std::memcpy(working, state, sizeof(state));

  for (int round = 0; round < 10; ++round) {
    quarter_round(working[0], working[4], working[8], working[12]);
    quarter_round(working[1], working[5], working[9], working[13]);
    quarter_round(working[2], working[6], working[10], working[14]);
    quarter_round(working[3], working[7], working[11], working[15]);
    quarter_round(working[0], working[5], working[10], working[15]);
    quarter_round(working[1], working[6], working[11], working[12]);
    quarter_round(working[2], working[7], working[8], working[13]);
    quarter_round(working[3], working[4], working[9], working[14]);
  }

  for (int i = 0; i < 16; ++i) store32_le(out + i * 4, working[i] + state[i]);
}

Bytes chacha20_xor(std::string_view key, std::uint32_t counter, std::string_view nonce, std::string_view data) {
  std::uint32_t key_words[8];
  for (int i = 0; i < 8; ++i) {
    key_words[i] = load32_le(reinterpret_cast<const std::uint8_t*>(key.data()) + i * 4);
  }
  std::uint32_t nonce_words[3] = {
      load32_le(reinterpret_cast<const std::uint8_t*>(nonce.data())),
      load32_le(reinterpret_cast<const std::uint8_t*>(nonce.data()) + 4),
      load32_le(reinterpret_cast<const std::uint8_t*>(nonce.data()) + 8),
  };

  Bytes output(data.size());
  std::uint8_t block[64];
  for (std::size_t offset = 0; offset < data.size(); offset += 64) {
    chacha20_block(key_words, counter + static_cast<std::uint32_t>(offset / 64), nonce_words, block);
    const std::size_t take = std::min<std::size_t>(64, data.size() - offset);
    for (std::size_t i = 0; i < take; ++i) {
      output[offset + i] = static_cast<std::uint8_t>(data[offset + i]) ^ block[i];
    }
  }
  return output;
}

std::array<std::uint8_t, 16> poly1305(std::string_view key, std::string_view message) {
  const auto* key_bytes = reinterpret_cast<const std::uint8_t*>(key.data());
  const std::uint32_t t0 = load32_le(key_bytes + 0);
  const std::uint32_t t1 = load32_le(key_bytes + 4);
  const std::uint32_t t2 = load32_le(key_bytes + 8);
  const std::uint32_t t3 = load32_le(key_bytes + 12);

  // r with the Poly1305 clamping; the masks are the "donna" limb splittings.
  const std::uint32_t r0 = t0 & 0x3ffffff;
  const std::uint32_t r1 = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
  const std::uint32_t r2 = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
  const std::uint32_t r3 = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
  const std::uint32_t r4 = (t3 >> 8) & 0x00fffff;

  const std::uint32_t s1 = r1 * 5;
  const std::uint32_t s2 = r2 * 5;
  const std::uint32_t s3 = r3 * 5;
  const std::uint32_t s4 = r4 * 5;

  std::uint32_t h0 = 0;
  std::uint32_t h1 = 0;
  std::uint32_t h2 = 0;
  std::uint32_t h3 = 0;
  std::uint32_t h4 = 0;

  const auto* bytes = reinterpret_cast<const std::uint8_t*>(message.data());
  std::size_t remaining = message.size();
  while (remaining > 0) {
    std::uint8_t block[17] = {0};
    const std::size_t take = std::min<std::size_t>(16, remaining);
    std::memcpy(block, bytes, take);
    block[take] = 1;  // the "1" bit appended to every block
    bytes += take;
    remaining -= take;

    h0 += static_cast<std::uint32_t>(block[0] | (block[1] << 8) | (block[2] << 16) | (block[3] << 24)) & 0x3ffffff;
    h1 += (load32_le(block + 3) >> 2) & 0x3ffffff;
    h2 += (load32_le(block + 6) >> 4) & 0x3ffffff;
    h3 += (load32_le(block + 9) >> 6) & 0x3ffffff;
    h4 += (load32_le(block + 12) >> 8) | (static_cast<std::uint32_t>(block[16]) << 24);

    const std::uint64_t d0 = static_cast<std::uint64_t>(h0) * r0 + static_cast<std::uint64_t>(h1) * s4 +
                             static_cast<std::uint64_t>(h2) * s3 + static_cast<std::uint64_t>(h3) * s2 +
                             static_cast<std::uint64_t>(h4) * s1;
    std::uint64_t d1 = static_cast<std::uint64_t>(h0) * r1 + static_cast<std::uint64_t>(h1) * r0 +
                       static_cast<std::uint64_t>(h2) * s4 + static_cast<std::uint64_t>(h3) * s3 +
                       static_cast<std::uint64_t>(h4) * s2;
    std::uint64_t d2 = static_cast<std::uint64_t>(h0) * r2 + static_cast<std::uint64_t>(h1) * r1 +
                       static_cast<std::uint64_t>(h2) * r0 + static_cast<std::uint64_t>(h3) * s4 +
                       static_cast<std::uint64_t>(h4) * s3;
    std::uint64_t d3 = static_cast<std::uint64_t>(h0) * r3 + static_cast<std::uint64_t>(h1) * r2 +
                       static_cast<std::uint64_t>(h2) * r1 + static_cast<std::uint64_t>(h3) * r0 +
                       static_cast<std::uint64_t>(h4) * s4;
    std::uint64_t d4 = static_cast<std::uint64_t>(h0) * r4 + static_cast<std::uint64_t>(h1) * r3 +
                       static_cast<std::uint64_t>(h2) * r2 + static_cast<std::uint64_t>(h3) * r1 +
                       static_cast<std::uint64_t>(h4) * r0;

    std::uint64_t carry = d0 >> 26;
    h0 = static_cast<std::uint32_t>(d0) & 0x3ffffff;
    d1 += carry;
    carry = d1 >> 26;
    h1 = static_cast<std::uint32_t>(d1) & 0x3ffffff;
    d2 += carry;
    carry = d2 >> 26;
    h2 = static_cast<std::uint32_t>(d2) & 0x3ffffff;
    d3 += carry;
    carry = d3 >> 26;
    h3 = static_cast<std::uint32_t>(d3) & 0x3ffffff;
    d4 += carry;
    carry = d4 >> 26;
    h4 = static_cast<std::uint32_t>(d4) & 0x3ffffff;
    h0 += static_cast<std::uint32_t>(carry) * 5;
    h1 += h0 >> 26;
    h0 &= 0x3ffffff;
  }

  // Final reduction.
  std::uint32_t carry = h1 >> 26;
  h1 &= 0x3ffffff;
  h2 += carry;
  carry = h2 >> 26;
  h2 &= 0x3ffffff;
  h3 += carry;
  carry = h3 >> 26;
  h3 &= 0x3ffffff;
  h4 += carry;
  carry = h4 >> 26;
  h4 &= 0x3ffffff;
  h0 += carry * 5;
  carry = h0 >> 26;
  h0 &= 0x3ffffff;
  h1 += carry;

  // g = h + 5 - 2^130, then pick g if g >= 0 (constant time).
  std::uint32_t g0 = h0 + 5;
  carry = g0 >> 26;
  g0 &= 0x3ffffff;
  std::uint32_t g1 = h1 + carry;
  carry = g1 >> 26;
  g1 &= 0x3ffffff;
  std::uint32_t g2 = h2 + carry;
  carry = g2 >> 26;
  g2 &= 0x3ffffff;
  std::uint32_t g3 = h3 + carry;
  carry = g3 >> 26;
  g3 &= 0x3ffffff;
  std::uint32_t g4 = h4 + carry - (1u << 26);

  const std::uint32_t mask = (g4 >> 31) - 1;  // all ones when g4 did not borrow
  const std::uint32_t unused = ~mask;
  (void)unused;
  h0 = (h0 & ~mask) | (g0 & mask);
  h1 = (h1 & ~mask) | (g1 & mask);
  h2 = (h2 & ~mask) | (g2 & mask);
  h3 = (h3 & ~mask) | (g3 & mask);
  h4 = (h4 & ~mask) | (g4 & mask);

  // h = h % 2^128
  const std::uint64_t f0 = (static_cast<std::uint64_t>(h0) | (static_cast<std::uint64_t>(h1) << 26)) & 0xffffffff;
  const std::uint64_t f1 =
      ((static_cast<std::uint64_t>(h1) >> 6) | (static_cast<std::uint64_t>(h2) << 20)) & 0xffffffff;
  const std::uint64_t f2 =
      ((static_cast<std::uint64_t>(h2) >> 12) | (static_cast<std::uint64_t>(h3) << 14)) & 0xffffffff;
  const std::uint64_t f3 =
      ((static_cast<std::uint64_t>(h3) >> 18) | (static_cast<std::uint64_t>(h4) << 8)) & 0xffffffff;

  // tag = (h + s) mod 2^128
  std::uint64_t sum = f0 + load32_le(key_bytes + 16);
  const std::uint32_t o0 = static_cast<std::uint32_t>(sum);
  sum = f1 + load32_le(key_bytes + 20) + (sum >> 32);
  const std::uint32_t o1 = static_cast<std::uint32_t>(sum);
  sum = f2 + load32_le(key_bytes + 24) + (sum >> 32);
  const std::uint32_t o2 = static_cast<std::uint32_t>(sum);
  sum = f3 + load32_le(key_bytes + 28) + (sum >> 32);
  const std::uint32_t o3 = static_cast<std::uint32_t>(sum);

  std::array<std::uint8_t, 16> tag{};
  store32_le(tag.data(), o0);
  store32_le(tag.data() + 4, o1);
  store32_le(tag.data() + 8, o2);
  store32_le(tag.data() + 12, o3);
  return tag;
}

Bytes chacha20_poly1305_seal(std::string_view key, std::string_view nonce, std::string_view aad,
                             std::string_view plaintext) {
  const Bytes poly_key = chacha20_xor(key, 0, nonce, std::string(64, '\0'));
  Bytes ciphertext = chacha20_xor(key, 1, nonce, plaintext);

  std::string mac_input;
  mac_input.append(aad);
  mac_input.append((16 - (aad.size() % 16)) % 16, '\0');  // pad16
  mac_input.append(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
  mac_input.append((16 - (ciphertext.size() % 16)) % 16, '\0');
  std::uint8_t lengths[16] = {0};
  const auto aad_size = static_cast<std::uint64_t>(aad.size());
  const auto data_size = static_cast<std::uint64_t>(ciphertext.size());
  for (int i = 0; i < 8; ++i) {
    lengths[i] = static_cast<std::uint8_t>(aad_size >> (8 * i));
    lengths[8 + i] = static_cast<std::uint8_t>(data_size >> (8 * i));
  }
  mac_input.append(reinterpret_cast<const char*>(lengths), sizeof(lengths));

  const std::array<std::uint8_t, 16> tag =
      poly1305(std::string_view(reinterpret_cast<const char*>(poly_key.data()), 32), mac_input);
  ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
  return ciphertext;
}

bool chacha20_poly1305_open(std::string_view key, std::string_view nonce, std::string_view aad,
                            std::string_view ciphertext_and_tag, Bytes& plaintext) {
  if (ciphertext_and_tag.size() < 16) return false;
  const std::string_view ciphertext = ciphertext_and_tag.substr(0, ciphertext_and_tag.size() - 16);
  const std::string_view tag = ciphertext_and_tag.substr(ciphertext_and_tag.size() - 16);

  const Bytes poly_key = chacha20_xor(key, 0, nonce, std::string(64, '\0'));
  std::string mac_input;
  mac_input.append(aad);
  mac_input.append((16 - (aad.size() % 16)) % 16, '\0');
  mac_input.append(ciphertext);
  mac_input.append((16 - (ciphertext.size() % 16)) % 16, '\0');
  std::uint8_t lengths[16] = {0};
  const auto aad_size = static_cast<std::uint64_t>(aad.size());
  const auto data_size = static_cast<std::uint64_t>(ciphertext.size());
  for (int i = 0; i < 8; ++i) {
    lengths[i] = static_cast<std::uint8_t>(aad_size >> (8 * i));
    lengths[8 + i] = static_cast<std::uint8_t>(data_size >> (8 * i));
  }
  mac_input.append(reinterpret_cast<const char*>(lengths), sizeof(lengths));

  const std::array<std::uint8_t, 16> expected =
      poly1305(std::string_view(reinterpret_cast<const char*>(poly_key.data()), 32), mac_input);
  if (!constant_time_equal(std::string_view(reinterpret_cast<const char*>(expected.data()), 16), tag)) {
    return false;
  }
  plaintext = chacha20_xor(key, 1, nonce, ciphertext);
  return true;
}

}  // namespace bonk::crypto
