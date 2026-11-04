#include <cstring>

#include "bonk/crypto/crypto.hpp"

namespace bonk::crypto {
namespace {

#if !defined(__SIZEOF_INT128__)
#error "X25519 needs a 128-bit intermediate type (GCC/Clang __int128)"
#endif
// `__extension__` keeps -Wpedantic quiet about the compiler extension we rely on.
__extension__ typedef __int128 wide_int;

constexpr std::uint64_t kMask51 = (1ULL << 51) - 1;
constexpr std::uint64_t kTwoP0 = 0xFFFFFFFFFFFDAULL;    // 2^52 - 38
constexpr std::uint64_t kTwoP1 = 0xFFFFFFFFFFFFEULL;    // 2^52 - 2

/// Field element modulo 2^255 - 19, five 51-bit limbs.
struct Fe {
  std::uint64_t v[5] = {0, 0, 0, 0, 0};
};

inline std::uint64_t load64(const std::uint8_t* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return value;
}

Fe fe_from_bytes(const std::uint8_t* bytes) {
  Fe h;
  h.v[0] = load64(bytes) & kMask51;
  h.v[1] = (load64(bytes + 6) >> 3) & kMask51;
  h.v[2] = (load64(bytes + 12) >> 6) & kMask51;
  h.v[3] = (load64(bytes + 19) >> 1) & kMask51;
  h.v[4] = (load64(bytes + 24) >> 12) & kMask51;
  return h;
}

void fe_reduce(Fe& h) {
  for (int pass = 0; pass < 2; ++pass) {
    std::uint64_t carry = h.v[0] >> 51;
    h.v[0] &= kMask51;
    h.v[1] += carry;
    carry = h.v[1] >> 51;
    h.v[1] &= kMask51;
    h.v[2] += carry;
    carry = h.v[2] >> 51;
    h.v[2] &= kMask51;
    h.v[3] += carry;
    carry = h.v[3] >> 51;
    h.v[3] &= kMask51;
    h.v[4] += carry;
    carry = h.v[4] >> 51;
    h.v[4] &= kMask51;
    h.v[0] += carry * 19;
  }
}

void fe_to_bytes(std::uint8_t* out, const Fe& input) {
  Fe h = input;
  fe_reduce(h);
  fe_reduce(h);

  // Canonical form: subtract p = 2^255 - 19 when h >= p.
  std::uint64_t g0 = h.v[0] + 19;
  std::uint64_t carry = g0 >> 51;
  g0 &= kMask51;
  std::uint64_t g1 = h.v[1] + carry;
  carry = g1 >> 51;
  g1 &= kMask51;
  std::uint64_t g2 = h.v[2] + carry;
  carry = g2 >> 51;
  g2 &= kMask51;
  std::uint64_t g3 = h.v[3] + carry;
  carry = g3 >> 51;
  g3 &= kMask51;
  const auto g4 = static_cast<std::int64_t>(h.v[4]) + static_cast<std::int64_t>(carry) -
                  static_cast<std::int64_t>(1ULL << 51);

  const std::uint64_t use_g = ~static_cast<std::uint64_t>(g4 >> 63);  // all ones when g4 >= 0

  h.v[0] = (h.v[0] & ~use_g) | (g0 & use_g);
  h.v[1] = (h.v[1] & ~use_g) | (g1 & use_g);
  h.v[2] = (h.v[2] & ~use_g) | (g2 & use_g);
  h.v[3] = (h.v[3] & ~use_g) | (g3 & use_g);
  h.v[4] = (h.v[4] & ~use_g) | (static_cast<std::uint64_t>(g4) & kMask51 & use_g);

  std::uint64_t words[4] = {0, 0, 0, 0};
  auto insert = [&words](std::uint64_t value, int bit) {
    const int word = bit / 64;
    const int shift = bit % 64;
    words[word] |= value << shift;
    if (shift != 0 && word + 1 < 4) words[word + 1] |= value >> (64 - shift);
  };
  insert(h.v[0], 0);
  insert(h.v[1], 51);
  insert(h.v[2], 102);
  insert(h.v[3], 153);
  insert(h.v[4], 204);

  for (int i = 0; i < 4; ++i) {
    for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<std::uint8_t>(words[i] >> (8 * b));
  }
}

inline Fe fe_add(const Fe& f, const Fe& g) {
  Fe h;
  for (int i = 0; i < 5; ++i) h.v[i] = f.v[i] + g.v[i];
  fe_reduce(h);
  h.v[4] &= kMask51;
  return h;
}

inline Fe fe_sub(const Fe& f, const Fe& g) {
  Fe h;
  h.v[0] = f.v[0] + kTwoP0 - g.v[0];
  h.v[1] = f.v[1] + kTwoP1 - g.v[1];
  h.v[2] = f.v[2] + kTwoP1 - g.v[2];
  h.v[3] = f.v[3] + kTwoP1 - g.v[3];
  h.v[4] = f.v[4] + kTwoP1 - g.v[4];
  fe_reduce(h);
  h.v[4] &= kMask51;
  return h;
}

inline Fe fe_mul(const Fe& f, const Fe& g) {
  const wide_int f0 = f.v[0];
  const wide_int f1 = f.v[1];
  const wide_int f2 = f.v[2];
  const wide_int f3 = f.v[3];
  const wide_int f4 = f.v[4];
  const wide_int g0 = g.v[0];
  const wide_int g1 = g.v[1];
  const wide_int g2 = g.v[2];
  const wide_int g3 = g.v[3];
  const wide_int g4 = g.v[4];

  wide_int t0 = f0 * g0 + 19 * (f1 * g4 + f2 * g3 + f3 * g2 + f4 * g1);
  wide_int t1 = f0 * g1 + f1 * g0 + 19 * (f2 * g4 + f3 * g3 + f4 * g2);
  wide_int t2 = f0 * g2 + f1 * g1 + f2 * g0 + 19 * (f3 * g4 + f4 * g3);
  const wide_int t3 = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + 19 * (f4 * g4);
  const wide_int t4 = f0 * g4 + f1 * g3 + f2 * g2 + f3 * g1 + f4 * g0;

  Fe h;
  std::uint64_t carry = static_cast<std::uint64_t>(t0 >> 51);
  h.v[0] = static_cast<std::uint64_t>(t0) & kMask51;
  t1 += carry;
  carry = static_cast<std::uint64_t>(t1 >> 51);
  h.v[1] = static_cast<std::uint64_t>(t1) & kMask51;
  t2 += carry;
  carry = static_cast<std::uint64_t>(t2 >> 51);
  h.v[2] = static_cast<std::uint64_t>(t2) & kMask51;
  const wide_int t3b = t3 + static_cast<wide_int>(carry);
  carry = static_cast<std::uint64_t>(t3b >> 51);
  h.v[3] = static_cast<std::uint64_t>(t3b) & kMask51;
  const wide_int t4b = t4 + static_cast<wide_int>(carry);
  carry = static_cast<std::uint64_t>(t4b >> 51);
  h.v[4] = static_cast<std::uint64_t>(t4b) & kMask51;
  h.v[0] += carry * 19;
  h.v[1] += h.v[0] >> 51;
  h.v[0] &= kMask51;
  return h;
}

inline Fe fe_sq(const Fe& f) { return fe_mul(f, f); }

inline Fe fe_mul_small(const Fe& f, std::uint64_t scalar) {
  wide_int t[5];
  for (int i = 0; i < 5; ++i) t[i] = static_cast<wide_int>(f.v[i]) * scalar;
  Fe h;
  std::uint64_t carry = 0;
  for (int i = 0; i < 5; ++i) {
    t[i] += carry;
    h.v[i] = static_cast<std::uint64_t>(t[i]) & kMask51;
    carry = static_cast<std::uint64_t>(t[i] >> 51);
  }
  h.v[0] += carry * 19;
  h.v[1] += h.v[0] >> 51;
  h.v[0] &= kMask51;
  return h;
}

inline void fe_cswap(Fe& a, Fe& b, std::uint64_t swap) {
  const std::uint64_t mask = 0 - swap;
  for (int i = 0; i < 5; ++i) {
    const std::uint64_t difference = mask & (a.v[i] ^ b.v[i]);
    a.v[i] ^= difference;
    b.v[i] ^= difference;
  }
}

Fe fe_invert(const Fe& z) {
  // z^(p-2) with p-2 = 2^255 - 21.
  Fe result;
  result.v[0] = 1;
  Fe base = z;
  std::uint8_t exponent[32];
  exponent[0] = 0x7F;
  for (int i = 1; i < 31; ++i) exponent[i] = 0xFF;
  exponent[31] = 0xEB;

  for (int bit = 254; bit >= 0; --bit) {
    result = fe_sq(result);
    const int byte = bit / 8;
    const int offset = bit % 8;
    if (((exponent[31 - byte] >> offset) & 1) != 0) result = fe_mul(result, base);
  }
  return result;
}

Fe fe_one() {
  Fe one;
  one.v[0] = 1;
  return one;
}

}  // namespace

std::array<std::uint8_t, 32> x25519(std::string_view private_key, std::string_view peer_public) {
  std::array<std::uint8_t, 32> scalar{};
  std::memcpy(scalar.data(), private_key.data(), 32);
  scalar[0] &= 248;
  scalar[31] &= 127;
  scalar[31] |= 64;

  std::array<std::uint8_t, 32> u_bytes{};
  std::memcpy(u_bytes.data(), peer_public.data(), 32);
  u_bytes[31] &= 127;  // RFC 7748: mask the high bit

  const Fe x1 = fe_from_bytes(u_bytes.data());
  Fe x2 = fe_one();
  Fe z2;  // zero
  Fe x3 = x1;
  Fe z3 = fe_one();
  std::uint64_t swap = 0;

  for (int t = 254; t >= 0; --t) {
    const std::uint64_t bit = (scalar[t / 8] >> (t % 8)) & 1;
    swap ^= bit;
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    swap = bit;

    const Fe a = fe_add(x2, z2);
    const Fe aa = fe_sq(a);
    const Fe b = fe_sub(x2, z2);
    const Fe bb = fe_sq(b);
    const Fe e = fe_sub(aa, bb);
    const Fe c = fe_add(x3, z3);
    const Fe d = fe_sub(x3, z3);
    const Fe da = fe_mul(d, a);
    const Fe cb = fe_mul(c, b);
    x3 = fe_sq(fe_add(da, cb));
    z3 = fe_mul(x1, fe_sq(fe_sub(da, cb)));
    x2 = fe_mul(aa, bb);
    z2 = fe_mul(e, fe_add(aa, fe_mul_small(e, 121665)));
  }
  fe_cswap(x2, x3, swap);
  fe_cswap(z2, z3, swap);

  const Fe result = fe_mul(x2, fe_invert(z2));
  std::array<std::uint8_t, 32> out{};
  fe_to_bytes(out.data(), result);
  return out;
}

std::array<std::uint8_t, 32> x25519_public_from_private(std::string_view private_key) {
  std::array<std::uint8_t, 32> base{};
  base[0] = 9;
  return x25519(private_key, std::string_view(reinterpret_cast<const char*>(base.data()), 32));
}

X25519KeyPair x25519_generate() {
  X25519KeyPair pair;
  const Bytes random = random_bytes(32);
  std::memcpy(pair.private_key.data(), random.data(), 32);
  pair.private_key[0] &= 248;
  pair.private_key[31] &= 127;
  pair.private_key[31] |= 64;
  pair.public_key = x25519_public_from_private(
      std::string_view(reinterpret_cast<const char*>(pair.private_key.data()), 32));
  return pair;
}

}  // namespace bonk::crypto
