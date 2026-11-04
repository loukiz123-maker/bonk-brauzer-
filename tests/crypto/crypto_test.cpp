#include <string>

#include "bonk/crypto/crypto.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

std::string hex_of(const std::array<std::uint8_t, 32>& bytes) {
  return crypto::to_hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string bytes_of(std::string_view hex) {
  const crypto::Bytes decoded = crypto::from_hex(hex);
  return std::string(reinterpret_cast<const char*>(decoded.data()), decoded.size());
}

}  // namespace

BONK_TEST(sha256, known_vectors) {
  CHECK_EQ(hex_of(crypto::sha256("")), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(hex_of(crypto::sha256("abc")), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(hex_of(crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // Incremental updates must match the one-shot hash (multi-block input).
  crypto::Sha256 hasher;
  const std::string chunk(1000, 'a');
  hasher.update(chunk);
  hasher.update(chunk);
  CHECK_EQ(hex_of(hasher.finish()), hex_of(crypto::sha256(chunk + chunk)));
}

BONK_TEST(hmac, rfc4231_vectors) {
  CHECK_EQ(hex_of(crypto::hmac_sha256(bytes_of("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"), "Hi There")),
           std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  CHECK_EQ(hex_of(crypto::hmac_sha256("Jefe", "what do ya want for nothing?")),
           std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

BONK_TEST(hkdf, rfc5869_case_1) {
  const std::array<std::uint8_t, 32> prk = crypto::hkdf_extract(bytes_of("000102030405060708090a0b0c"),
                                                 bytes_of("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"));
  CHECK_EQ(crypto::to_hex(std::string_view(reinterpret_cast<const char*>(prk.data()), prk.size())),
           std::string("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
  const crypto::Bytes okm =
      crypto::hkdf_expand(std::string_view(reinterpret_cast<const char*>(prk.data()), prk.size()),
                          bytes_of("f0f1f2f3f4f5f6f7f8f9"), 42);
  CHECK_EQ(crypto::to_hex(std::string_view(reinterpret_cast<const char*>(okm.data()), okm.size())),
           std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
}

BONK_TEST(chacha20, rfc8439_block_and_stream) {
  const std::string key = bytes_of("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const std::string nonce = bytes_of("000000090000004a00000000");

  std::uint32_t key_words[8];
  for (int i = 0; i < 8; ++i) {
    key_words[i] = static_cast<std::uint32_t>(static_cast<std::uint8_t>(key[i * 4])) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(key[i * 4 + 1])) << 8) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(key[i * 4 + 2])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(key[i * 4 + 3])) << 24);
  }
  std::uint32_t nonce_words[3] = {0x09000000u, 0x4a000000u, 0u};
  std::uint8_t block[64];
  crypto::chacha20_block(key_words, 1, nonce_words, block);
  const std::string block_hex = crypto::to_hex(std::string_view(reinterpret_cast<const char*>(block), 64));
  CHECK_EQ(block_hex,
           std::string("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                       "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e"));

  // Section 2.4.2 (second example): nonce 00..00 4a 00..00, counter 1.
  const std::string stream_nonce = bytes_of("000000000000004a00000000");
  const std::string plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
  const std::string ciphertext = bytes_of(
      "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b357"
      "1639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
      "5af90bbf74a35be6b40b8eedf2785e42874d");
  const crypto::Bytes stream = crypto::chacha20_xor(key, 1, stream_nonce, plaintext);
  CHECK_EQ(crypto::to_hex(std::string_view(reinterpret_cast<const char*>(stream.data()), stream.size())),
           crypto::to_hex(ciphertext));
}

BONK_TEST(poly1305, rfc8439_vectors) {
  const std::string key = bytes_of("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
  const std::array<std::uint8_t, 16> tag = crypto::poly1305(key, "Cryptographic Forum Research Group");
  CHECK_EQ(crypto::to_hex(std::string_view(reinterpret_cast<const char*>(tag.data()), tag.size())),
           std::string("a8061dc1305136c6c22b8baf0c0127a9"));
}

BONK_TEST(aead, rfc8439_full_vector) {
  // RFC 8439 §2.8.2: the complete AEAD vector (padding and length encoding included).
  const std::string key = bytes_of("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const std::string nonce = bytes_of("070000004041424344454647");
  const std::string aad = bytes_of("50515253c0c1c2c3c4c5c6c7");
  const std::string plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
  const crypto::Bytes sealed = crypto::chacha20_poly1305_seal(key, nonce, aad, plaintext);
  CHECK_EQ(crypto::to_hex(std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size())),
           std::string("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
                       "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
                       "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                       "3ff4def08e4b7a9de576d26586cec64b6116"
                       "1ae10b594f09e26a7e902ecbd0600691"));
}

BONK_TEST(aead, seal_and_open_round_trip) {
  const std::string key = bytes_of("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const std::string nonce = bytes_of("070000004041424344454647");
  const std::string aad = bytes_of("50515253c0c1c2c3c4c5c6c7");

  const crypto::Bytes sealed = crypto::chacha20_poly1305_seal(key, nonce, aad, "hello bonk");
  crypto::Bytes opened;
  REQUIRE(crypto::chacha20_poly1305_open(key, nonce, aad,
                                        std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size()),
                                        opened));
  CHECK_EQ(std::string(reinterpret_cast<const char*>(opened.data()), opened.size()), std::string("hello bonk"));

  // Any bit flip in the tag must be rejected.
  crypto::Bytes tampered = sealed;
  tampered.back() = static_cast<std::uint8_t>(tampered.back() ^ 0x01);
  CHECK(!crypto::chacha20_poly1305_open(
      key, nonce, aad, std::string_view(reinterpret_cast<const char*>(tampered.data()), tampered.size()), opened));
  // ... and in the AAD too.
  std::string wrong_aad = aad;
  wrong_aad[0] = static_cast<char>(wrong_aad[0] ^ 0x01);
  CHECK(!crypto::chacha20_poly1305_open(
      key, nonce, wrong_aad, std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size()), opened));
}

BONK_TEST(x25519, rfc7748_scalarmult_vector) {
  const std::string scalar = bytes_of("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
  const std::string point = bytes_of("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
  const std::array<std::uint8_t, 32> shared = crypto::x25519(scalar, point);
  CHECK_EQ(hex_of(shared), std::string("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
}

BONK_TEST(x25519, rfc7748_base_point_and_diffie_hellman) {
  // Alice's public key from RFC 7748 §6.1.
  const std::string alice_private = bytes_of("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  CHECK_EQ(hex_of(crypto::x25519_public_from_private(alice_private)),
           std::string("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));

  const std::string bob_private = bytes_of("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  const std::string bob_public =
      hex_of(crypto::x25519_public_from_private(bob_private));
  CHECK_EQ(bob_public, std::string("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));

  const std::array<std::uint8_t, 32> alice_shared = crypto::x25519(alice_private, bytes_of(bob_public));
  const std::array<std::uint8_t, 32> bob_shared = crypto::x25519(
      bob_private, bytes_of("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
  CHECK_EQ(hex_of(alice_shared), hex_of(bob_shared));
  CHECK_EQ(hex_of(alice_shared), std::string("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
}

BONK_TEST(x25519, generated_keypairs_agree) {
  const crypto::X25519KeyPair a = crypto::x25519_generate();
  const crypto::X25519KeyPair b = crypto::x25519_generate();
  CHECK(a.public_key != b.public_key);
  const std::array<std::uint8_t, 32> shared_ab = crypto::x25519(
      std::string_view(reinterpret_cast<const char*>(a.private_key.data()), 32),
      std::string_view(reinterpret_cast<const char*>(b.public_key.data()), 32));
  const std::array<std::uint8_t, 32> shared_ba = crypto::x25519(
      std::string_view(reinterpret_cast<const char*>(b.private_key.data()), 32),
      std::string_view(reinterpret_cast<const char*>(a.public_key.data()), 32));
  CHECK(shared_ab == shared_ba);
  CHECK(shared_ab[0] != 0 || shared_ab[1] != 0);  // not the all-zero "small order" result
}

BONK_TEST(crypto, helpers) {
  const crypto::Bytes random = crypto::random_bytes(48);
  CHECK_EQ(random.size(), std::size_t{48});
  CHECK(random != crypto::random_bytes(48));  // astronomically unlikely to collide

  CHECK(crypto::constant_time_equal("abc", "abc"));
  CHECK(!crypto::constant_time_equal("abc", "abd"));
  CHECK(!crypto::constant_time_equal("abc", "ab"));

  CHECK_EQ(crypto::to_hex(std::string_view("\x00\xff", 2)), std::string("00ff"));
  CHECK_EQ(crypto::from_hex("00FF").size(), std::size_t{2});
}

BONK_TEST(hkdf, tls13_expand_label_shape) {
  // The label is wrapped as "tls13 " + label, length is a 2-byte prefix: check that
  // asking for N bytes yields exactly N and that labels change the output.
  const std::string secret(32, '\x01');
  const crypto::Bytes key = crypto::hkdf_expand_label(secret, "key", 32);
  const crypto::Bytes iv = crypto::hkdf_expand_label(secret, "iv", 12);
  CHECK_EQ(key.size(), std::size_t{32});
  CHECK_EQ(iv.size(), std::size_t{12});
  CHECK(key != crypto::Bytes(32, 0));
  CHECK(crypto::hkdf_expand_label(secret, "key", 16) != crypto::hkdf_expand_label(secret, "iv", 16));
}
