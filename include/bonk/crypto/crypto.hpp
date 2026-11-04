// Bonk Brauzer — cryptography written from scratch (no OpenSSL, no libsodium).
//
// Scope: exactly what TLS 1.3 needs for our cipher suite
// TLS_CHACHA20_POLY1305_SHA256 with the x25519 key exchange:
//   * SHA-256 / HMAC-SHA256 / HKDF (RFC 5869) + TLS 1.3 HKDF-Expand-Label
//   * ChaCha20-Poly1305 AEAD (RFC 8439)
//   * X25519 (RFC 7748) with constant-time Montgomery ladder
//   * OS randomness via getrandom()/"/dev/urandom"
// Not constant-time: SHA-256 (not needed: no secret-dependent branches) — the
// 25519 ladder and Poly1305 are branch-free; ChaCha20 is armless by construction.
// Not implemented: AES-GCM (we simply do not offer AES suites), RSA, ECDSA (cert
// signatures are parsed but not yet verified — see docs/STAGE-02B.md).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bonk::crypto {

using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------- hashing ----

struct Sha256 {
  std::array<std::uint32_t, 8> state{};
  std::array<std::uint8_t, 64> block{};
  std::uint64_t total = 0;
  std::size_t block_size = 0;

  Sha256();
  void update(const void* data, std::size_t length);
  void update(std::string_view text) { update(text.data(), text.size()); }
  [[nodiscard]] std::array<std::uint8_t, 32> finish();
};

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data);
[[nodiscard]] std::array<std::uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data);

// ------------------------------------------------------------------ HKDF -----

/// RFC 5869 extract: PRK = HMAC(salt, ikm).
[[nodiscard]] std::array<std::uint8_t, 32> hkdf_extract(std::string_view salt, std::string_view ikm);
/// RFC 5869 expand; `length` must be <= 255*32.
[[nodiscard]] Bytes hkdf_expand(std::string_view prk, std::string_view info, std::size_t length);
/// TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1).
[[nodiscard]] Bytes hkdf_expand_label(std::string_view secret, std::string_view label, std::size_t length);
/// TLS 1.3 HKDF-Expand-Label with an explicit context (used for "finished").
[[nodiscard]] Bytes hkdf_expand_label_with_context(std::string_view secret, std::string_view label,
                                                   std::string_view context, std::size_t length);

// ------------------------------------------------------ ChaCha20-Poly1305 ----

/// RFC 8439 §2.4 ChaCha20 block function (counter || nonce).
void chacha20_block(const std::uint32_t key[8], std::uint32_t counter, const std::uint32_t nonce[3],
                    std::uint8_t out[64]);
/// RFC 8439 §2.4 encryption (used for the AEAD keystream as well).
[[nodiscard]] Bytes chacha20_xor(std::string_view key, std::uint32_t counter, std::string_view nonce,
                                 std::string_view data);
/// RFC 8439 §2.5 Poly1305 one-shot MAC.
[[nodiscard]] std::array<std::uint8_t, 16> poly1305(std::string_view key, std::string_view message);

/// RFC 8439 §2.8 AEAD; `nonce` is 12 bytes. Output = ciphertext || tag.
[[nodiscard]] Bytes chacha20_poly1305_seal(std::string_view key, std::string_view nonce, std::string_view aad,
                                           std::string_view plaintext);
/// Verifies the tag in constant time; empty result on failure.
[[nodiscard]] bool chacha20_poly1305_open(std::string_view key, std::string_view nonce, std::string_view aad,
                                          std::string_view ciphertext_and_tag, Bytes& plaintext);

// -------------------------------------------------------------- X25519 ------

struct X25519KeyPair {
  std::array<std::uint8_t, 32> private_key{};  // already clamped
  std::array<std::uint8_t, 32> public_key{};
};

[[nodiscard]] X25519KeyPair x25519_generate();
[[nodiscard]] std::array<std::uint8_t, 32> x25519(std::string_view private_key, std::string_view peer_public);
/// Convenience wrapper used by the TLS client.
[[nodiscard]] std::array<std::uint8_t, 32> x25519_public_from_private(std::string_view private_key);

// -------------------------------------------------------------- helpers -----

[[nodiscard]] Bytes random_bytes(std::size_t count);
void constant_time_copy(void* destination, const void* source, std::size_t length);
[[nodiscard]] bool constant_time_equal(std::string_view a, std::string_view b);
[[nodiscard]] std::string to_hex(std::string_view bytes);
[[nodiscard]] Bytes from_hex(std::string_view hex);

}  // namespace bonk::crypto
