#include <cstdlib>
#include <fstream>
#include <string>

#include "bonk/crypto/crypto.hpp"
#include "bonk/net/http.hpp"
#include "bonk/net/tls.hpp"
#include "bonk/net/x509.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

std::string read_binary(const std::string& path) {
  std::ifstream file{path, std::ios::binary};
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
  std::ifstream file{path};
  return static_cast<bool>(file);
}

Result<net::Certificate> fixture_certificate() { return net::parse_certificate(read_binary("tests/fixtures/test-cert.der")); }

/// Builds a gzip stream around `payload` using stored deflate blocks.
std::string gzip_of(std::string_view payload) {
  std::string out("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\x03", 10);
  out.push_back(static_cast<char>(0x01));  // final stored block
  const auto length = static_cast<std::uint16_t>(payload.size());
  out.push_back(static_cast<char>(length & 0xFF));
  out.push_back(static_cast<char>((length >> 8) & 0xFF));
  out.push_back(static_cast<char>(~length & 0xFF));
  out.push_back(static_cast<char>((~length >> 8) & 0xFF));
  out.append(payload);

  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : payload) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  crc ^= 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((crc >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((payload.size() >> (8 * i)) & 0xFF));
  return out;
}

}  // namespace

BONK_TEST(x509, parses_the_fixture_certificate) {
  REQUIRE(file_exists("tests/fixtures/test-cert.der"));
  const Result<net::Certificate> certificate = fixture_certificate();
  REQUIRE(certificate.is_ok());
  const net::Certificate& parsed = certificate.value();
  CHECK_EQ(parsed.subject_common_name, std::string("localhost"));
  CHECK_EQ(parsed.issuer_common_name, std::string("localhost"));
  CHECK_EQ(parsed.dns_names.size(), std::size_t{1});
  CHECK_EQ(parsed.dns_names.front(), std::string("localhost"));
  CHECK(parsed.not_before.size() >= 20);  // "YYYY-MM-DDTHH:MM:SSZ"
  CHECK(parsed.not_after > parsed.not_before);
  CHECK(!parsed.serial_hex.empty());
}

BONK_TEST(x509, hostname_matching_and_wildcards) {
  net::Certificate certificate;
  certificate.dns_names = {"example.com", "*.wild.example"};
  CHECK(net::certificate_matches_host(certificate, "example.com"));
  CHECK(net::certificate_matches_host(certificate, "EXAMPLE.com"));
  CHECK(net::certificate_matches_host(certificate, "api.wild.example"));
  CHECK(!net::certificate_matches_host(certificate, "a.b.wild.example"));  // wildcard is one label only
  CHECK(!net::certificate_matches_host(certificate, "wild.example"));
  CHECK(!net::certificate_matches_host(certificate, "example.net"));
}

BONK_TEST(x509, validity_window_is_enforced) {
  net::Certificate certificate;
  certificate.not_before = "2025-01-01T00:00:00Z";
  certificate.not_after = "2026-01-01T00:00:00Z";
  CHECK(net::certificate_is_valid_at(certificate, "2025-06-01T00:00:00Z"));
  CHECK(!net::certificate_is_valid_at(certificate, "2024-12-31T23:59:59Z"));
  CHECK(!net::certificate_is_valid_at(certificate, "2026-01-02T00:00:00Z"));
}

BONK_TEST(x509, rejects_garbage) {
  CHECK(net::parse_certificate("not a certificate").is_error());
  CHECK(net::parse_certificate("").is_error());
}

BONK_TEST(gzip, decodes_a_stored_block_stream) {
  const std::string payload = "bonk test payload";
  const Result<std::string> decoded = net::decode_gzip(gzip_of(payload));
  REQUIRE(decoded.is_ok());
  CHECK_EQ(decoded.value(), payload);
}

BONK_TEST(gzip, detects_corruption) {
  std::string stream = gzip_of("hello gzip");
  CHECK(net::decode_gzip("").is_error());
  stream[0] = 0x00;  // break the magic
  CHECK(net::decode_gzip(stream).is_error());

  std::string good = gzip_of("hello gzip");
  good.back() = static_cast<char>(good.back() ^ 0xFF);  // corrupt the size trailer
  CHECK(net::decode_gzip(good).is_error());
}

BONK_TEST(tls, key_schedule_is_deterministic_and_transcript_sensitive) {
  const std::string handshake_secret(32, '\x11');
  const std::string transcript(32, '\x22');
  const crypto::Bytes first = crypto::hkdf_expand_label_with_context(handshake_secret, "c hs traffic", transcript, 32);
  const crypto::Bytes second = crypto::hkdf_expand_label_with_context(handshake_secret, "c hs traffic", transcript, 32);
  CHECK(first == second);

  std::string other_transcript = transcript;
  other_transcript[0] = static_cast<char>(other_transcript[0] ^ 0x01);
  const crypto::Bytes third =
      crypto::hkdf_expand_label_with_context(handshake_secret, "c hs traffic", other_transcript, 32);
  CHECK(first != third);

  // The traffic keys are 32-byte key + 12-byte IV, derived with the right labels.
  const crypto::Bytes key = crypto::hkdf_expand_label(std::string_view(reinterpret_cast<const char*>(first.data()), 32),
                                                      "key", 32);
  const crypto::Bytes iv = crypto::hkdf_expand_label(std::string_view(reinterpret_cast<const char*>(first.data()), 32),
                                                     "iv", 12);
  CHECK_EQ(key.size(), std::size_t{32});
  CHECK_EQ(iv.size(), std::size_t{12});
}

BONK_TEST(tls, record_layer_round_trip_with_sequence_numbers) {
  // Mirror of the record protection in src/net/tls.cpp: same key/nonce/AD rules.
  const std::string key(32, '\x5A');
  std::array<std::uint8_t, 12> iv{};
  iv.fill(0x0F);

  auto seal = [&](std::uint64_t sequence, std::string_view plaintext) {
    std::array<std::uint8_t, 12> nonce = iv;
    for (int i = 0; i < 8; ++i) {
      nonce[11 - i] = static_cast<std::uint8_t>(nonce[11 - i] ^ ((sequence >> (8 * i)) & 0xFF));
    }
    std::string inner(plaintext);
    inner.push_back(23);
    const std::string header("\x17\x03\x03\x00\x00", 5);
    return crypto::chacha20_poly1305_seal(key, std::string_view(reinterpret_cast<const char*>(nonce.data()), 12),
                                          header, inner);
  };

  const crypto::Bytes first = seal(0, "one");
  const crypto::Bytes second = seal(1, "one");
  CHECK(first != second);  // the nonce depends on the record sequence number

  std::array<std::uint8_t, 12> nonce = iv;  // sequence 0
  crypto::Bytes opened;
  REQUIRE(crypto::chacha20_poly1305_open(key, std::string_view(reinterpret_cast<const char*>(nonce.data()), 12),
                                         std::string_view("\x17\x03\x03\x00\x00", 5),
                                         std::string_view(reinterpret_cast<const char*>(first.data()), first.size()),
                                         opened));
  CHECK_EQ(std::string(reinterpret_cast<const char*>(opened.data()), opened.size()), std::string("one\x17"));
}

BONK_TEST(tls, live_handshake_to_github_is_optional) {
  // Network tests are opt-in: set BONK_TEST_NET=1 to run them (CI stays offline).
  const char* enabled = std::getenv("BONK_TEST_NET");
  if (enabled == nullptr || std::string(enabled) != "1") {
    ::bonk::test::record_note("skipped live TLS test (BONK_TEST_NET != 1)");
    return;
  }
  Result<std::unique_ptr<net::TlsConnection>> connection = net::TlsConnection::connect("github.com", 443, 8000);
  REQUIRE(connection.is_ok());
  const net::TlsPeerInfo& peer = connection.value()->peer();
  CHECK_EQ(peer.negotiated_version, std::string("TLS 1.3"));
  CHECK(peer.finished_verified);
  CHECK(peer.hostname_matches);
  CHECK(peer.certificate_in_validity_window);
  CHECK(!peer.chain_verified);  // documented gap

  REQUIRE(connection.value()
              ->write("GET / HTTP/1.1\r\nHost: github.com\r\nConnection: close\r\nUser-Agent: bonk\r\n\r\n")
              .is_ok());
  std::string response;
  for (int i = 0; i < 32; ++i) {
    const Result<std::string> chunk = connection.value()->read(8000);
    if (chunk.is_error()) break;
    if (chunk.value().empty()) break;
    response += chunk.value();
    if (response.size() > 65536) break;
  }
  CHECK(response.starts_with("HTTP/1.1 200"));
}

BONK_TEST(tls, https_request_through_the_http_layer_is_optional) {
  const char* enabled = std::getenv("BONK_TEST_NET");
  if (enabled == nullptr || std::string(enabled) != "1") {
    ::bonk::test::record_note("skipped live HTTPS test (BONK_TEST_NET != 1)");
    return;
  }
  const Result<net::Url> url = net::parse_url("https://github.com/");
  REQUIRE(url.is_ok());
  Diagnostics diagnostics;
  const Result<net::HttpResponse> response = net::http_get(url.value(), &diagnostics, 10000);
  REQUIRE(response.is_ok());
  CHECK_EQ(response.value().status, 200);
  CHECK(response.value().body.size() > 1000);
}
