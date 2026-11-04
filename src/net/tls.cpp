#include "bonk/net/tls.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bonk/crypto/crypto.hpp"

namespace bonk::net {
namespace {

constexpr std::uint8_t kContentChangeCipherSpec = 20;
constexpr std::uint8_t kContentAlert = 21;
constexpr std::uint8_t kContentHandshake = 22;
constexpr std::uint8_t kContentApplicationData = 23;

constexpr std::uint16_t kCipherChaCha20Poly1305Sha256 = 0x1303;
constexpr std::uint16_t kGroupX25519 = 0x001D;
constexpr std::size_t kMaxRecordPayload = 16384 + 256;
constexpr std::size_t kMacLength = 16;

Error tls_error(std::string_view code, std::string_view message) { return Error::make(code, message); }

/// A tiny writer that keeps TLS's uint8/uint16/uint24 length prefixes honest.
struct Writer {
  std::string data;

  void u8(std::uint8_t value) { data.push_back(static_cast<char>(value)); }
  void u16(std::uint16_t value) {
    data.push_back(static_cast<char>((value >> 8) & 0xFF));
    data.push_back(static_cast<char>(value & 0xFF));
  }
  void u24(std::uint32_t value) {
    data.push_back(static_cast<char>((value >> 16) & 0xFF));
    data.push_back(static_cast<char>((value >> 8) & 0xFF));
    data.push_back(static_cast<char>(value & 0xFF));
  }
  void raw(std::string_view bytes) { data.append(bytes); }
  void raw(const std::vector<std::uint8_t>& bytes) {
    data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  void raw(const std::array<std::uint8_t, 32>& bytes) {
    data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  void vector8(std::string_view bytes) {
    u8(static_cast<std::uint8_t>(bytes.size()));
    raw(bytes);
  }
  void vector16(std::string_view bytes) {
    u16(static_cast<std::uint16_t>(bytes.size()));
    raw(bytes);
  }
  void vector24(std::string_view bytes) {
    u24(static_cast<std::uint32_t>(bytes.size()));
    raw(bytes);
  }
  [[nodiscard]] std::size_t size() const { return data.size(); }
};

/// Reads big-endian length-prefixed fields; every accessor is bounds checked.
struct Reader {
  std::string_view data;
  std::size_t position = 0;
  bool failed = false;

  [[nodiscard]] bool available(std::size_t count) const { return position + count <= data.size(); }
  std::uint8_t u8() {
    if (!available(1)) {
      failed = true;
      return 0;
    }
    return static_cast<std::uint8_t>(data[position++]);
  }
  std::uint16_t u16() {
    if (!available(2)) {
      failed = true;
      return 0;
    }
    const auto value = static_cast<std::uint16_t>((static_cast<std::uint8_t>(data[position]) << 8) |
                                                  static_cast<std::uint8_t>(data[position + 1]));
    position += 2;
    return value;
  }
  std::uint32_t u24() {
    if (!available(3)) {
      failed = true;
      return 0;
    }
    const std::uint32_t value = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[position])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[position + 1])) << 8) |
                                static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[position + 2]));
    position += 3;
    return value;
  }
  std::string_view bytes(std::size_t count) {
    if (!available(count)) {
      failed = true;
      return {};
    }
    const std::string_view slice = data.substr(position, count);
    position += count;
    return slice;
  }
  std::string_view vector8() { return bytes(u8()); }
  std::string_view vector16() { return bytes(u16()); }
  std::string_view vector24() { return bytes(u24()); }
  [[nodiscard]] std::size_t remaining() const { return data.size() - position; }
};

std::array<std::uint8_t, 32> transcript_hash(const std::string& transcript) {
  return crypto::sha256(transcript);
}

void xor_nonce(std::array<std::uint8_t, 12>& nonce, std::uint64_t sequence) {
  for (int i = 0; i < 8; ++i) {
    nonce[11 - i] = static_cast<std::uint8_t>(nonce[11 - i] ^ ((sequence >> (8 * i)) & 0xFF));
  }
}

}  // namespace

TlsConnection::~TlsConnection() { close(); }

void TlsConnection::close() {
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
}

Status TlsConnection::send_all(std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t written = ::send(socket_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (written <= 0) {
      if (written < 0 && errno == EINTR) continue;
      return Status::failure(tls_error("net.tls_send_failed", "the connection was closed while sending"));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::success();
}

Result<std::string> TlsConnection::receive(std::size_t max_bytes, int timeout_ms) {
  std::string buffer(max_bytes, '\0');
  std::size_t received = 0;
  while (received < max_bytes) {
    pollfd descriptor{socket_, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready == 0) {
      return Result<std::string>::failure(tls_error("net.tls_timeout", "timed out waiting for the server"));
    }
    if (ready < 0) {
      if (errno == EINTR) continue;
      return Result<std::string>::failure(tls_error("net.tls_read_failed", "poll failed"));
    }
    const ssize_t got = ::recv(socket_, buffer.data() + received, max_bytes - received, 0);
    if (got == 0) break;  // EOF
    if (got < 0) {
      if (errno == EINTR) continue;
      return Result<std::string>::failure(tls_error("net.tls_read_failed", std::strerror(errno)));
    }
    received += static_cast<std::size_t>(got);
  }
  buffer.resize(received);
  return Result<std::string>::success(std::move(buffer));
}

Status TlsConnection::write_record(std::uint8_t type, std::string_view fragment) {
  std::string record;
  record.push_back(static_cast<char>(type));
  record.push_back(0x03);  // legacy record version 0x0301 for the first flight, 0x0303 afterwards
  record.push_back(0x03);
  record.push_back(static_cast<char>((fragment.size() >> 8) & 0xFF));
  record.push_back(static_cast<char>(fragment.size() & 0xFF));
  record.append(fragment);
  return send_all(record);
}

Status TlsConnection::write_encrypted(std::uint8_t inner_type, std::string_view data, TrafficKeys& keys) {
  std::string inner(data);
  inner.push_back(static_cast<char>(inner_type));
  const std::size_t ciphertext_length = inner.size() + kMacLength;

  std::string header;
  header.push_back(static_cast<char>(kContentApplicationData));
  header.push_back(0x03);
  header.push_back(0x03);
  header.push_back(static_cast<char>((ciphertext_length >> 8) & 0xFF));
  header.push_back(static_cast<char>(ciphertext_length & 0xFF));

  std::array<std::uint8_t, 12> nonce = keys.iv;
  xor_nonce(nonce, keys.sequence);
  ++keys.sequence;

  const crypto::Bytes sealed = crypto::chacha20_poly1305_seal(
      std::string_view(reinterpret_cast<const char*>(keys.key.data()), keys.key.size()),
      std::string_view(reinterpret_cast<const char*>(nonce.data()), nonce.size()), header, inner);

  std::string record = header;
  record.append(reinterpret_cast<const char*>(sealed.data()), sealed.size());
  return send_all(record);
}

Status TlsConnection::read_record(int timeout_ms, TrafficKeys* keys, std::uint8_t& inner_type, std::string& payload) {
  Result<std::string> header_result = receive(5, timeout_ms);
  if (header_result.is_error()) return Status::failure(header_result.error());
  const std::string& header = header_result.value();
  if (header.empty()) return Status::failure(tls_error("net.tls_eof", "the server closed the connection"));

  const auto type = static_cast<std::uint8_t>(header[0]);
  const std::size_t length = (static_cast<std::size_t>(static_cast<std::uint8_t>(header[3])) << 8) |
                             static_cast<std::uint8_t>(header[4]);
  if (length > kMaxRecordPayload) {
    return Status::failure(tls_error("net.tls_bad_record", "record is too large"));
  }

  Result<std::string> body_result = receive(length, timeout_ms);
  if (body_result.is_error()) return Status::failure(body_result.error());
  if (body_result.value().size() != length) {
    return Status::failure(tls_error("net.tls_eof", "the connection ended inside a record"));
  }
  const std::string& body = body_result.value();

  if (type == kContentChangeCipherSpec) {  // middlebox compatibility garbage
    inner_type = kContentChangeCipherSpec;
    payload.clear();
    return Status::success();
  }

  if (type == kContentAlert) {
    if (body.size() >= 2) {
      return Status::failure(tls_error("net.tls_alert", "the server sent an alert: description " +
                                                            std::to_string(static_cast<std::uint8_t>(body[1]))));
    }
    return Status::failure(tls_error("net.tls_alert", "the server sent an alert"));
  }

  if (keys == nullptr) {
    if (type != kContentHandshake) {
      return Status::failure(tls_error("net.tls_unexpected_record", "expected a plaintext handshake record"));
    }
    inner_type = type;
    payload = body;
    return Status::success();
  }

  if (type != kContentApplicationData) {
    return Status::failure(tls_error("net.tls_unexpected_record", "expected an encrypted record"));
  }
  if (body.size() <= kMacLength) return Status::failure(tls_error("net.tls_bad_record", "record is too short"));

  std::array<std::uint8_t, 12> nonce = keys->iv;
  xor_nonce(nonce, keys->sequence);
  ++keys->sequence;

  crypto::Bytes plaintext;
  if (!crypto::chacha20_poly1305_open(
          std::string_view(reinterpret_cast<const char*>(keys->key.data()), keys->key.size()),
          std::string_view(reinterpret_cast<const char*>(nonce.data()), nonce.size()), header, body, plaintext)) {
    return Status::failure(tls_error("net.tls_bad_tag", "record authentication failed"));
  }

  // Strip the zero padding, then the inner content type.
  std::size_t end = plaintext.size();
  while (end > 0 && plaintext[end - 1] == 0) --end;
  if (end == 0) return Status::failure(tls_error("net.tls_bad_record", "record has no inner type"));
  inner_type = plaintext[end - 1];
  payload.assign(reinterpret_cast<const char*>(plaintext.data()), end - 1);
  return Status::success();
}

Status TlsConnection::send_handshake_message(std::string_view message, TrafficKeys* keys, std::uint8_t inner_type) {
  if (keys == nullptr) return write_record(kContentHandshake, message);
  return write_encrypted(inner_type, message, *keys);
}

Result<std::string> TlsConnection::read_handshake_messages(int timeout_ms) {
  for (;;) {
    if (handshake_buffer_.size() >= 4) {
      const std::size_t length = (static_cast<std::size_t>(static_cast<std::uint8_t>(handshake_buffer_[1])) << 16) |
                                 (static_cast<std::size_t>(static_cast<std::uint8_t>(handshake_buffer_[2])) << 8) |
                                 static_cast<std::uint8_t>(handshake_buffer_[3]);
      if (handshake_buffer_.size() >= length + 4) {
        std::string message = handshake_buffer_.substr(0, length + 4);
        handshake_buffer_.erase(0, length + 4);
        return Result<std::string>::success(std::move(message));
      }
    }
    std::uint8_t inner_type = 0;
    std::string payload;
    const Status status = read_record(timeout_ms, &server_handshake_keys_, inner_type, payload);
    if (status.is_error()) return Result<std::string>::failure(status.error());
    if (inner_type == kContentChangeCipherSpec) continue;
    if (inner_type != kContentHandshake) {
      return Result<std::string>::failure(
          tls_error("net.tls_unexpected_record", "expected encrypted handshake data"));
    }
    handshake_buffer_ += payload;
  }
}

Result<std::string> TlsConnection::read_plain_handshake(int timeout_ms) {
  for (;;) {
    if (handshake_buffer_.size() >= 4) {
      const std::size_t length = (static_cast<std::size_t>(static_cast<std::uint8_t>(handshake_buffer_[1])) << 16) |
                                 (static_cast<std::size_t>(static_cast<std::uint8_t>(handshake_buffer_[2])) << 8) |
                                 static_cast<std::uint8_t>(handshake_buffer_[3]);
      if (handshake_buffer_.size() >= length + 4) {
        std::string message = handshake_buffer_.substr(0, length + 4);
        handshake_buffer_.erase(0, length + 4);
        return Result<std::string>::success(std::move(message));
      }
    }
    std::uint8_t inner_type = 0;
    std::string payload;
    const Status status = read_record(timeout_ms, nullptr, inner_type, payload);
    if (status.is_error()) return Result<std::string>::failure(status.error());
    if (inner_type == kContentChangeCipherSpec) continue;
    handshake_buffer_ += payload;
  }
}

Result<std::unique_ptr<TlsConnection>> TlsConnection::connect(std::string_view host, std::uint16_t port,
                                                              int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* resolved = nullptr;
  const std::string host_string(host);
  if (::getaddrinfo(host_string.c_str(), std::to_string(port).c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return Result<std::unique_ptr<TlsConnection>>::failure(
        tls_error("net.dns_failure", "cannot resolve '" + host_string + "'"));
  }

  int socket = -1;
  std::string last_error = "no address worked for '" + host_string + "'";
  for (addrinfo* entry = resolved; entry != nullptr; entry = entry->ai_next) {
    socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket < 0) continue;

    const int flags = ::fcntl(socket, F_GETFL, 0);
    ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    int connected = ::connect(socket, entry->ai_addr, entry->ai_addrlen);
    if (connected != 0) {
      if (errno != EINPROGRESS) {
        last_error = std::strerror(errno);
        ::close(socket);
        socket = -1;
        continue;
      }
      pollfd descriptor{socket, POLLOUT, 0};
      const int ready = ::poll(&descriptor, 1, timeout_ms);
      if (ready <= 0) {
        last_error = "connect timed out";
        ::close(socket);
        socket = -1;
        continue;
      }
      int error = 0;
      socklen_t error_size = sizeof(error);
      ::getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &error_size);
      if (error != 0) {
        last_error = std::strerror(error);
        ::close(socket);
        socket = -1;
        continue;
      }
    }
    ::fcntl(socket, F_SETFL, flags);  // blocking again; we implement timeouts with poll()
    break;
  }
  ::freeaddrinfo(resolved);
  if (socket < 0) {
    return Result<std::unique_ptr<TlsConnection>>::failure(tls_error("net.connect_failed", last_error));
  }

  std::unique_ptr<TlsConnection> connection(new TlsConnection());
  connection->socket_ = socket;
  connection->timeout_ms_ = timeout_ms;
  connection->peer_.server_name = host_string;

  const Status handshake = connection->handshake(host);
  if (handshake.is_error()) {
    const Error error = handshake.error();
    connection->close();
    return Result<std::unique_ptr<TlsConnection>>::failure(error);
  }
  connection->handshake_complete_ = true;
  return Result<std::unique_ptr<TlsConnection>>::success(std::move(connection));
}

TlsConnection::TrafficKeys TlsConnection::derive_keys(std::string_view secret) {
  TrafficKeys keys;
  const crypto::Bytes key = crypto::hkdf_expand_label(secret, "key", 32);
  const crypto::Bytes iv = crypto::hkdf_expand_label(secret, "iv", 12);
  std::memcpy(keys.key.data(), key.data(), std::min<std::size_t>(key.size(), keys.key.size()));
  std::memcpy(keys.iv.data(), iv.data(), std::min<std::size_t>(iv.size(), keys.iv.size()));
  return keys;
}

Status TlsConnection::process_certificate_message(std::string_view body) {
  Reader reader{body, 0, false};
  (void)reader.vector8();  // certificate_request_context
  const std::string_view list = reader.vector24();
  if (reader.failed || list.empty()) {
    return Status::failure(tls_error("net.tls_bad_certificate", "the server sent no certificate"));
  }

  Reader entries{list, 0, false};
  std::size_t index = 0;
  std::string leaf_der;
  while (entries.remaining() > 0) {
    const std::string_view der = entries.vector24();
    if (entries.failed) break;
    if (index == 0) leaf_der = std::string(der);
    ++index;
  }
  peer_.certificate_count = index;
  if (leaf_der.empty()) {
    return Status::failure(tls_error("net.tls_bad_certificate", "could not read the leaf certificate"));
  }

  Result<Certificate> parsed = parse_certificate(leaf_der);
  if (parsed.is_error()) return Status::failure(parsed.error());
  peer_.leaf = std::move(parsed.value());
  return Status::success();
}

Status TlsConnection::handshake(std::string_view host) {
  const crypto::X25519KeyPair keypair = crypto::x25519_generate();
  const crypto::Bytes client_random = crypto::random_bytes(32);
  const crypto::Bytes session_id = crypto::random_bytes(32);

  // ---- ClientHello ---------------------------------------------------------
  Writer extensions;
  {
    Writer sni_entry;
    sni_entry.u8(0);  // host_name
    sni_entry.vector16(host);
    Writer list;
    list.vector16(sni_entry.data);  // ServerNameList
    Writer extension;
    extension.u16(0);  // server_name
    extension.vector16(list.data);
    extensions.raw(extension.data);
  }
  {
    Writer groups;
    groups.u16(kGroupX25519);
    Writer list;
    list.vector16(groups.data);  // extension_data is a length-prefixed vector
    Writer extension;
    extension.u16(10);  // supported_groups
    extension.vector16(list.data);
    extensions.raw(extension.data);
  }
  {
    Writer algorithms;
    for (const std::uint16_t algorithm : {std::uint16_t{0x0403}, std::uint16_t{0x0804}, std::uint16_t{0x0401},
                                          std::uint16_t{0x0503}}) {
      algorithms.u16(algorithm);
    }
    Writer list;
    list.vector16(algorithms.data);
    Writer extension;
    extension.u16(13);  // signature_algorithms
    extension.vector16(list.data);
    extensions.raw(extension.data);
  }
  {
    // extension_data = u8 length + version list (the length byte is part of the data)
    Writer versions;
    versions.u8(2);
    versions.u16(0x0304);  // TLS 1.3
    Writer extension;
    extension.u16(43);  // supported_versions
    extension.vector16(versions.data);
    extensions.raw(extension.data);
  }
  {
    Writer shares;
    shares.u16(kGroupX25519);
    shares.vector16(std::string_view(reinterpret_cast<const char*>(keypair.public_key.data()),
                                     keypair.public_key.size()));
    Writer list;
    list.vector16(shares.data);
    Writer extension;
    extension.u16(51);  // key_share
    extension.vector16(list.data);
    extensions.raw(extension.data);
  }

  Writer hello;
  hello.u16(0x0303);  // legacy_version
  hello.raw(client_random);
  hello.vector8(std::string_view(reinterpret_cast<const char*>(session_id.data()), session_id.size()));
  hello.vector16(std::string_view("\x13\x03", 2));  // TLS_CHACHA20_POLY1305_SHA256
  hello.vector8(std::string_view("\x00", 1));       // null compression
  hello.vector16(extensions.data);

  Writer client_hello;
  client_hello.u8(1);
  client_hello.u24(static_cast<std::uint32_t>(hello.size()));
  client_hello.raw(hello.data);
  transcript_ += client_hello.data;

  if (std::getenv("BONK_TLS_DEBUG") != nullptr) {
    std::fprintf(stderr, "ClientHello (%zu bytes): %s\n", client_hello.size(), crypto::to_hex(client_hello.data).c_str());
  }
  Status status = write_record(kContentHandshake, client_hello.data);
  if (status.is_error()) return status;

  // ---- ServerHello ---------------------------------------------------------
  Result<std::string> server_hello_result = read_plain_handshake(timeout_ms_);
  if (server_hello_result.is_error()) return Status::failure(server_hello_result.error());
  const std::string server_hello = std::move(server_hello_result.value());
  if (static_cast<std::uint8_t>(server_hello[0]) != 2) {
    return Status::failure(tls_error("net.tls_bad_handshake", "expected ServerHello"));
  }
  transcript_ += server_hello;

  std::string server_public_key;
  {
    Reader reader{std::string_view(server_hello).substr(4), 0, false};
    (void)reader.u16();  // legacy_version
    (void)reader.bytes(32);  // server random
    (void)reader.vector8();  // legacy session id echo
    const std::uint16_t cipher = reader.u16();
    (void)reader.u8();  // compression
    const std::string_view extension_block = reader.vector16();
    if (reader.failed) {
      return Status::failure(tls_error("net.tls_bad_handshake", "malformed ServerHello"));
    }
    if (cipher != kCipherChaCha20Poly1305Sha256) {
      return Status::failure(tls_error("net.tls_unsupported_cipher",
                                       "the server chose cipher suite 0x" + crypto::to_hex(std::string_view(
                                                                                  reinterpret_cast<const char*>(&cipher),
                                                                                  2)) +
                                           "; only ChaCha20-Poly1305/SHA-256 is implemented"));
    }

    bool version_ok = false;
    Reader extensions_reader{extension_block, 0, false};
    while (extensions_reader.remaining() > 0 && !extensions_reader.failed) {
      const std::uint16_t type = extensions_reader.u16();
      const std::string_view value = extensions_reader.vector16();
      if (type == 43) {  // supported_versions
        Reader version_reader{value, 0, false};
        version_ok = version_reader.u16() == 0x0304;
      } else if (type == 51) {  // key_share
        Reader share_reader{value, 0, false};
        const std::uint16_t group = share_reader.u16();
        const std::string_view key = share_reader.vector16();
        if (group != kGroupX25519 || key.size() != 32) {
          return Status::failure(tls_error("net.tls_unsupported_group", "the server did not agree on x25519"));
        }
        server_public_key = std::string(key);
      }
    }
    if (!version_ok) return Status::failure(tls_error("net.tls_version", "the server did not negotiate TLS 1.3"));
  }
  if (server_public_key.empty()) {
    return Status::failure(tls_error("net.tls_bad_handshake", "the ServerHello has no key share"));
  }

  // ---- Key schedule (RFC 8446 §7.1) ---------------------------------------
  const std::array<std::uint8_t, 32> shared = crypto::x25519(
      std::string_view(reinterpret_cast<const char*>(keypair.private_key.data()), keypair.private_key.size()),
      server_public_key);
  const std::string zeros(32, '\0');
  // Derive-Secret(Secret, "derived", "") hashes the empty transcript, so the
  // context is Hash(""), not an empty string (RFC 8446 §7.1).
  const std::array<std::uint8_t, 32> empty_hash = crypto::sha256(std::string_view{});
  const std::string_view empty_hash_view(reinterpret_cast<const char*>(empty_hash.data()), empty_hash.size());

  const std::array<std::uint8_t, 32> early_secret = crypto::hkdf_extract(std::string_view{}, zeros);
  const crypto::Bytes derived = crypto::hkdf_expand_label_with_context(
      std::string_view(reinterpret_cast<const char*>(early_secret.data()), early_secret.size()), "derived",
      empty_hash_view, 32);
  const std::array<std::uint8_t, 32> handshake_secret = crypto::hkdf_extract(
      std::string_view(reinterpret_cast<const char*>(derived.data()), derived.size()),
      std::string_view(reinterpret_cast<const char*>(shared.data()), shared.size()));

  const std::array<std::uint8_t, 32> hello_hash = transcript_hash(transcript_);
  const std::string hello_hash_string(reinterpret_cast<const char*>(hello_hash.data()), hello_hash.size());
  const std::string handshake_secret_string(reinterpret_cast<const char*>(handshake_secret.data()),
                                            handshake_secret.size());

  const crypto::Bytes client_hs_secret =
      crypto::hkdf_expand_label_with_context(handshake_secret_string, "c hs traffic", hello_hash_string, 32);
  const crypto::Bytes server_hs_secret =
      crypto::hkdf_expand_label_with_context(handshake_secret_string, "s hs traffic", hello_hash_string, 32);
  client_handshake_keys_ = derive_keys(std::string_view(reinterpret_cast<const char*>(client_hs_secret.data()),
                                                       client_hs_secret.size()));
  server_handshake_keys_ = derive_keys(std::string_view(reinterpret_cast<const char*>(server_hs_secret.data()),
                                                       server_hs_secret.size()));

  // Middlebox compatibility: a dummy ChangeCipherSpec in the clear (RFC 8446 §5.1).
  status = write_record(kContentChangeCipherSpec, std::string_view("\x01", 1));
  if (status.is_error()) return status;

  // ---- Encrypted server flight --------------------------------------------
  bool got_finished = false;
  for (;;) {
    Result<std::string> message_result = read_handshake_messages(timeout_ms_);
    if (message_result.is_error()) return Status::failure(message_result.error());
    const std::string message = std::move(message_result.value());
    const auto type = static_cast<std::uint8_t>(message[0]);
    const std::string_view body(message.data() + 4, message.size() - 4);

    if (type == 20) {  // Finished: verify before trusting anything else
      const crypto::Bytes finished_key = crypto::hkdf_expand_label(
          std::string_view(reinterpret_cast<const char*>(server_hs_secret.data()), server_hs_secret.size()),
          "finished", 32);
      const std::array<std::uint8_t, 32> expected_hash = transcript_hash(transcript_);
      const std::array<std::uint8_t, 32> expected = crypto::hmac_sha256(
          std::string_view(reinterpret_cast<const char*>(finished_key.data()), finished_key.size()),
          std::string_view(reinterpret_cast<const char*>(expected_hash.data()), expected_hash.size()));
      if (!crypto::constant_time_equal(
              std::string_view(reinterpret_cast<const char*>(expected.data()), expected.size()), body)) {
        return Status::failure(tls_error("net.tls_bad_finished", "the server's Finished was not valid"));
      }
      peer_.finished_verified = true;
      transcript_ += message;
      got_finished = true;
      break;
    }

    if (type == 11) {  // Certificate
      status = process_certificate_message(body);
      if (status.is_error()) return status;
    } else if (type == 15) {
      // CertificateVerify: parsed, signature verification arrives with the chain
      // builder (stage 6). Recorded in peer_.chain_verified == false.
      (void)peer_.leaf.dns_names.size();
    }
    transcript_ += message;
  }
  if (!got_finished) return Status::failure(tls_error("net.tls_bad_handshake", "the server never finished"));

  // Certificate policy: we can check the hostname and the validity window today.
  peer_.hostname_checked = true;
  peer_.hostname_matches = certificate_matches_host(peer_.leaf, host);
  peer_.certificate_in_validity_window = certificate_is_valid_at(peer_.leaf);
  if (!peer_.hostname_matches) {
    return Status::failure(tls_error("net.tls_hostname_mismatch",
                                     "the certificate is not valid for '" + std::string(host) + "'"));
  }
  if (!peer_.certificate_in_validity_window) {
    return Status::failure(tls_error("net.tls_certificate_expired", "the certificate is outside its validity window"));
  }

  // ---- Application keys ----------------------------------------------------
  const crypto::Bytes derived2 = crypto::hkdf_expand_label_with_context(handshake_secret_string, "derived",
                                                                      empty_hash_view, 32);
  const std::array<std::uint8_t, 32> master_secret =
      crypto::hkdf_extract(std::string_view(reinterpret_cast<const char*>(derived2.data()), derived2.size()), zeros);
  const std::string master_string(reinterpret_cast<const char*>(master_secret.data()), master_secret.size());

  const std::array<std::uint8_t, 32> full_hash = transcript_hash(transcript_);
  const std::string full_hash_string(reinterpret_cast<const char*>(full_hash.data()), full_hash.size());
  const crypto::Bytes client_ap_secret =
      crypto::hkdf_expand_label_with_context(master_string, "c ap traffic", full_hash_string, 32);
  const crypto::Bytes server_ap_secret =
      crypto::hkdf_expand_label_with_context(master_string, "s ap traffic", full_hash_string, 32);
  client_application_keys_ = derive_keys(std::string_view(reinterpret_cast<const char*>(client_ap_secret.data()),
                                                        client_ap_secret.size()));
  server_application_keys_ = derive_keys(std::string_view(reinterpret_cast<const char*>(server_ap_secret.data()),
                                                        server_ap_secret.size()));

  // ---- Client Finished -----------------------------------------------------
  std::array<std::uint8_t, 32> transcript_now = transcript_hash(transcript_);
  const crypto::Bytes client_finished_key = crypto::hkdf_expand_label(
      std::string_view(reinterpret_cast<const char*>(client_hs_secret.data()), client_hs_secret.size()), "finished", 32);
  const std::array<std::uint8_t, 32> verify_data = crypto::hmac_sha256(
      std::string_view(reinterpret_cast<const char*>(client_finished_key.data()), client_finished_key.size()),
      std::string_view(reinterpret_cast<const char*>(transcript_now.data()), transcript_now.size()));

  Writer finished;
  finished.u8(20);
  finished.u24(32);
  finished.raw(verify_data);
  status = send_handshake_message(finished.data, &client_handshake_keys_, kContentHandshake);
  if (status.is_error()) return status;
  transcript_ += finished.data;

  peer_.cipher_suite = "TLS_CHACHA20_POLY1305_SHA256";
  peer_.key_exchange = "x25519";
  peer_.negotiated_version = "TLS 1.3";
  return Status::success();
}

Status TlsConnection::write(std::string_view data) {
  if (socket_ < 0) return Status::failure(tls_error("net.tls_closed", "the connection is closed"));
  if (!handshake_complete_) return Status::failure(tls_error("net.tls_not_ready", "the handshake is not complete"));
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t chunk = std::min<std::size_t>(kMaxRecordPayload - 64, data.size() - offset);
    const Status status = write_encrypted(kContentApplicationData, data.substr(offset, chunk), client_application_keys_);
    if (status.is_error()) return status;
    offset += chunk;
  }
  return Status::success();
}

Result<std::string> TlsConnection::read(int timeout_ms) {
  if (socket_ < 0) return Result<std::string>::success(std::string{});
  for (;;) {
    std::uint8_t inner_type = 0;
    std::string payload;
    const Status status = read_record(timeout_ms, &server_application_keys_, inner_type, payload);
    if (status.is_error()) {
      if (status.error().code == "net.tls_eof") return Result<std::string>::success(std::string{});
      return Result<std::string>::failure(status.error());
    }
    if (inner_type == kContentApplicationData) return Result<std::string>::success(std::move(payload));
    if (inner_type == kContentHandshake) continue;  // NewSessionTicket etc. — ignore
  }
}

}  // namespace bonk::net
