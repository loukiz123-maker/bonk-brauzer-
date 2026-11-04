// Bonk Brauzer — TLS 1.3 client written from scratch.
//
// Cipher suite: TLS_CHACHA20_POLY1305_SHA256 (0x1303) with X25519 key exchange.
// No OpenSSL, no mbedTLS: the record layer, the key schedule (RFC 8446 §7.1) and
// the handshake state machine live in src/net/tls.cpp on top of bonk::crypto.
//
// Deliberate scope of this stage:
//   * full handshake: ClientHello → ServerHello → EncryptedExtensions →
//     Certificate → CertificateVerify → Finished, plus our Finished;
//   * Finished is *verified* (HMAC over the transcript), so the handshake cannot
//     be silently tampered with;
//   * the leaf certificate is parsed and the hostname + validity window are
//     checked; chain-of-trust signature verification is NOT implemented yet, and
//     `TlsPeerInfo::chain_verified` says so explicitly instead of pretending.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"
#include "bonk/net/x509.hpp"

namespace bonk::net {

struct TlsPeerInfo {
  std::string server_name;          // SNI we sent
  std::string cipher_suite;         // "TLS_CHACHA20_POLY1305_SHA256"
  std::string key_exchange;         // "x25519"
  std::string negotiated_version;   // "TLS 1.3"
  Certificate leaf;                 // parsed leaf certificate (best effort)
  std::size_t certificate_count = 0;
  bool hostname_checked = false;
  bool hostname_matches = false;
  bool certificate_in_validity_window = false;
  bool chain_verified = false;      // always false until stage 6 (documented gap)
  bool finished_verified = false;   // true: the server proved possession of the handshake keys
};

class TlsConnection {
 public:
  ~TlsConnection();
  TlsConnection(const TlsConnection&) = delete;
  TlsConnection& operator=(const TlsConnection&) = delete;

  /// Opens a TCP connection and completes the handshake.
  static Result<std::unique_ptr<TlsConnection>> connect(std::string_view host, std::uint16_t port,
                                                        int timeout_ms = 8000);

  /// Sends application data as one or more TLS records.
  Status write(std::string_view data);

  /// Returns the next chunk of application data; an empty string means EOF.
  Result<std::string> read(int timeout_ms = 8000);

  void close();
  [[nodiscard]] const TlsPeerInfo& peer() const noexcept { return peer_; }
  [[nodiscard]] bool closed() const noexcept { return socket_ < 0; }

 private:
  TlsConnection() = default;

  struct TrafficKeys {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 12> iv{};
    std::uint64_t sequence = 0;
  };

  Status handshake(std::string_view host);
  Result<std::string> read_plain_handshake(int timeout_ms);
  Status process_certificate_message(std::string_view body);
  [[nodiscard]] TrafficKeys derive_keys(std::string_view secret);
  Status send_all(std::string_view data);
  Result<std::string> receive(std::size_t max_bytes, int timeout_ms);
  Status write_record(std::uint8_t type, std::string_view fragment);
  Status write_encrypted(std::uint8_t inner_type, std::string_view data, TrafficKeys& keys);
  /// Reads one record; decrypts when `keys` is not null.
  Status read_record(int timeout_ms, TrafficKeys* keys, std::uint8_t& inner_type, std::string& payload);
  Result<std::string> read_handshake_messages(int timeout_ms);
  Status send_handshake_message(std::string_view message, TrafficKeys* keys, std::uint8_t inner_type);

  int socket_ = -1;
  TrafficKeys client_handshake_keys_;
  TrafficKeys server_handshake_keys_;
  TrafficKeys client_application_keys_;
  TrafficKeys server_application_keys_;
  std::string transcript_;
  std::string handshake_buffer_;
  TlsPeerInfo peer_;
  bool handshake_complete_ = false;
  int timeout_ms_ = 8000;
};

}  // namespace bonk::net
