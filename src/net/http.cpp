#include "bonk/net/http.hpp"

#include <algorithm>
#include <cstring>

#include "bonk/core/strings.hpp"
#include "bonk/net/tls.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bonk::net {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle handle) { closesocket(handle); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle handle) { ::close(handle); }
#endif

struct SocketGuard {
  SocketHandle handle = kInvalidSocket;
  ~SocketGuard() {
    if (handle != kInvalidSocket) close_socket(handle);
  }
};

Error net_error(std::string_view code, std::string_view message) { return Error::make(code, message); }

Result<SocketHandle> connect_to(std::string_view host, std::uint16_t port, int timeout_ms) {
#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return Result<SocketHandle>::failure(net_error("net.wsa", "WSAStartup failed"));
  }
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string host_string(host);
  const std::string port_string = std::to_string(port);
  addrinfo* resolved = nullptr;
  const int lookup = ::getaddrinfo(host_string.c_str(), port_string.c_str(), &hints, &resolved);
  if (lookup != 0 || resolved == nullptr) {
    return Result<SocketHandle>::failure(
        net_error("net.dns_failure", "cannot resolve '" + host_string + "': " + gai_strerror(lookup)));
  }

  Result<SocketHandle> result = Result<SocketHandle>::failure(net_error("net.connect_failed", "no address worked"));
  for (addrinfo* entry = resolved; entry != nullptr; entry = entry->ai_next) {
    SocketHandle handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocket) continue;

    // Non-blocking connect + poll gives us a real timeout instead of a hang.
#if defined(_WIN32)
    u_long non_blocking = 1;
    ioctlsocket(handle, FIONBIO, &non_blocking);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
    int connected = ::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
    if (connected != 0) {
      pollfd descriptor{};
      descriptor.fd = handle;
      descriptor.events = POLLOUT;
      const int ready = ::poll(&descriptor, 1, timeout_ms);
      if (ready <= 0) {
        close_socket(handle);
        continue;
      }
      int so_error = 0;
      socklen_t length = sizeof(so_error);
      ::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &length);
      if (so_error != 0) {
        close_socket(handle);
        continue;
      }
    }
#if !defined(_WIN32)
    const int flags_after = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, flags_after & ~O_NONBLOCK);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    result = Result<SocketHandle>::success(handle);
    break;
  }
  ::freeaddrinfo(resolved);
  return result;
}

Result<std::string> read_all(SocketHandle handle, Diagnostics* diagnostics) {
  std::string response;
  char buffer[16384];
  for (;;) {
    const auto received = ::recv(handle, buffer, sizeof(buffer), 0);
    if (received == 0) break;
    if (received < 0) {
      if (diagnostics != nullptr) diagnostics->warn(SourcePos{}, "net.read_error: connection reset while reading");
      break;
    }
    response.append(buffer, static_cast<std::size_t>(received));
    if (response.size() > 64u * 1024u * 1024u) {
      return Result<std::string>::failure(net_error("net.response_too_large", "response exceeds 64 MiB"));
    }
  }
  if (response.empty()) {
    return Result<std::string>::failure(net_error("net.empty_response", "server closed the connection"));
  }
  return Result<std::string>::success(std::move(response));
}

}  // namespace

std::string_view HttpResponse::header(std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (equals_ci(key, name)) return value;
  }
  return {};
}

Result<std::string> decode_chunked(std::string_view body) {
  std::string out;
  std::size_t position = 0;
  for (;;) {
    const std::size_t line_end = body.find("\r\n", position);
    if (line_end == std::string_view::npos) return Result<std::string>::failure(net_error("net.bad_chunk", "truncated chunk header"));
    std::string_view size_text = body.substr(position, line_end - position);
    const std::size_t extension = size_text.find(';');
    if (extension != std::string_view::npos) size_text = size_text.substr(0, extension);
    size_text = trim(size_text);

    unsigned long long size = 0;
    try {
      size = std::stoull(std::string(size_text), nullptr, 16);
    } catch (...) {
      return Result<std::string>::failure(net_error("net.bad_chunk", "invalid chunk size"));
    }
    position = line_end + 2;
    if (size == 0) return Result<std::string>::success(std::move(out));
    if (position + size > body.size()) {
      return Result<std::string>::failure(net_error("net.bad_chunk", "chunk runs past the end of the body"));
    }
    out.append(body.substr(position, size));
    position += static_cast<std::size_t>(size) + 2;  // skip CRLF
  }
}

std::string build_request(const HttpRequest& request) {
  std::string out;
  out += request.method;
  out += ' ';
  out += request.url.has_opaque_path() ? request.url.path : request.url.path;
  if (out.back() == ' ') out += '/';
  if (!request.url.query.empty()) {
    out += '?';
    out += request.url.query;
  }
  out += " HTTP/1.1\r\nHost: ";
  out += request.url.host;
  if (request.url.port.has_value() && request.url.effective_port() != request.url.port.value_or(0)) {
    out += ':';
    out += std::to_string(*request.url.port);
  }
  out += "\r\nUser-Agent: BonkBrauzer/0.1\r\nAccept: text/html,*/*\r\nAccept-Encoding: gzip, deflate, identity\r\nConnection: close\r\n";
  if (request.cookie_jar != nullptr) {
    const std::string cookies = request.cookie_jar->cookie_header(request.url);
    if (!cookies.empty()) {
      out += "Cookie: ";
      out += cookies;
      out += "\r\n";
    }
  }
  if (!request.body.empty() || request.method == "POST") {
    out += "Content-Length: ";
    out += std::to_string(request.body.size());
    out += "\r\nContent-Type: ";
    out += request.content_type.empty() ? std::string("application/x-www-form-urlencoded") : request.content_type;
    out += "\r\n";
  }
  for (const auto& [name, value] : request.headers) {
    out += name;
    out += ": ";
    out += value;
    out += "\r\n";
  }
  out += "\r\n";
  out += request.body;
  return out;
}

Result<HttpResponse> http_request(const HttpRequest& request, Diagnostics* diagnostics) {
  Url current = request.url;
  int redirects = 0;

  for (;;) {
    const bool secure = current.scheme == "https" || current.scheme == "wss";
    if (!secure && current.scheme != "http" && current.scheme != "ws") {
      return Result<HttpResponse>::failure(
          net_error("net.unsupported_scheme", "scheme '" + current.scheme + "' is not supported"));
    }
    if (current.host.empty()) {
      return Result<HttpResponse>::failure(net_error("net.missing_host", "URL has no host"));
    }

    HttpRequest current_request = request;
    current_request.url = current;

    const std::string wire = build_request(current_request);
    Result<std::string> raw = Result<std::string>::failure(net_error("net.internal", "no transport"));

    if (secure) {
      // HTTPS: our own TLS 1.3 client (src/net/tls.cpp).
      Result<std::unique_ptr<TlsConnection>> tls =
          TlsConnection::connect(current.host, current.effective_port(), request.timeout_ms);
      if (tls.is_error()) return Result<HttpResponse>::failure(tls.error());
      if (diagnostics != nullptr) {
        const TlsPeerInfo& peer = tls.value()->peer();
        diagnostics->add(Severity::Hint, SourcePos{},
                         "net.tls: " + peer.negotiated_version + " " + peer.cipher_suite + " " + peer.key_exchange +
                             (peer.chain_verified ? " (chain verified)" : " (certificate chain NOT verified yet)"));
      }
      const Status sent_status = tls.value()->write(wire);
      if (sent_status.is_error()) return Result<HttpResponse>::failure(sent_status.error());

      std::string accumulated;
      for (;;) {
        Result<std::string> chunk = tls.value()->read(request.timeout_ms);
        if (chunk.is_error()) {
          if (chunk.error().code == "net.tls_timeout" && !accumulated.empty()) break;  // server keeps the socket open
          return Result<HttpResponse>::failure(chunk.error());
        }
        if (chunk.value().empty()) break;
        accumulated += chunk.value();
        if (accumulated.size() > 64u * 1024u * 1024u) break;
      }
      raw = Result<std::string>::success(std::move(accumulated));
    } else {
      Result<SocketHandle> socket = connect_to(current.host, current.effective_port(), request.timeout_ms);
      if (socket.is_error()) return Result<HttpResponse>::failure(socket.error());
      SocketGuard guard{socket.value()};

      std::size_t sent = 0;
      while (sent < wire.size()) {
        const auto written = ::send(guard.handle, wire.data() + sent, static_cast<int>(wire.size() - sent), 0);
        if (written <= 0) {
          return Result<HttpResponse>::failure(net_error("net.send_failed", "could not send the request"));
        }
        sent += static_cast<std::size_t>(written);
      }
      raw = read_all(guard.handle, diagnostics);
    }
    if (raw.is_error()) return Result<HttpResponse>::failure(raw.error());

    const std::string& response = raw.value();
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      return Result<HttpResponse>::failure(net_error("net.bad_response", "no header terminator in the response"));
    }
    const std::string_view head(response.data(), header_end);
    std::string_view body(response.data() + header_end + 4, response.size() - header_end - 4);

    HttpResponse parsed;
    parsed.final_url = current.serialize();
    parsed.redirect_count = redirects;

    const std::size_t first_line_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, first_line_end);
    const std::vector<std::string_view> status_parts = split(status_line, ' ');
    if (status_parts.size() < 2) {
      return Result<HttpResponse>::failure(net_error("net.bad_status_line", std::string(status_line)));
    }
    unsigned long long status = 0;
    if (!parse_u64(status_parts[1], status)) {
      return Result<HttpResponse>::failure(net_error("net.bad_status_line", std::string(status_line)));
    }
    parsed.status = static_cast<int>(status);
    if (status_parts.size() > 2) {
      std::size_t start = status_line.find(status_parts[2]);
      parsed.reason = std::string(status_line.substr(start));
    }

    std::size_t position = first_line_end == std::string_view::npos ? head.size() : first_line_end + 2;
    while (position < head.size()) {
      const std::size_t line_end = head.find("\r\n", position);
      const std::string_view line =
          head.substr(position, line_end == std::string_view::npos ? head.size() - position : line_end - position);
      const std::size_t colon = line.find(':');
      if (colon != std::string_view::npos) {
        parsed.headers.emplace_back(std::string(trim(line.substr(0, colon))),
                                    std::string(trim(line.substr(colon + 1))));
      }
      if (line_end == std::string_view::npos) break;
      position = line_end + 2;
    }

    if (request.cookie_jar != nullptr) {
      for (const auto& [name, value] : parsed.headers) {
        if (name == "set-cookie" || name == "Set-Cookie") (void)request.cookie_jar->store_from_header(current, value);
      }
    }

    const bool is_redirect = parsed.status == 301 || parsed.status == 302 || parsed.status == 303 ||
                             parsed.status == 307 || parsed.status == 308;
    if (is_redirect && redirects < request.max_redirects) {
      const std::string_view location = parsed.header("location");
      if (!location.empty()) {
        Result<Url> next = parse_url(location, &current);
        if (next.is_error()) return Result<HttpResponse>::failure(next.error());
        current = std::move(next.value());
        ++redirects;
        if (diagnostics != nullptr) {
          diagnostics->add(Severity::Hint, SourcePos{}, "net.redirect: " + current.serialize());
        }
        continue;
      }
    }

    if (const std::string_view encoding = parsed.header("transfer-encoding");
        encoding.find("chunked") != std::string_view::npos) {
      Result<std::string> decoded = decode_chunked(body);
      if (decoded.is_error()) return Result<HttpResponse>::failure(decoded.error());
      parsed.body = std::move(decoded.value());
    } else if (const std::string_view length = parsed.header("content-length"); !length.empty()) {
      unsigned long long expected = 0;
      if (parse_u64(length, expected) && expected <= body.size()) {
        parsed.body.assign(body.substr(0, static_cast<std::size_t>(expected)));
      } else {
        parsed.body.assign(body);
      }
    } else {
      parsed.body.assign(body);
    }

    if (const std::string_view content_encoding = parsed.header("content-encoding");
        !content_encoding.empty() && !equals_ci(content_encoding, "identity")) {
      Result<std::string> decoded_body = Result<std::string>::success(std::string{});
      if (equals_ci(content_encoding, "gzip") || equals_ci(content_encoding, "x-gzip")) {
        decoded_body = decode_gzip(parsed.body);
      } else if (equals_ci(content_encoding, "deflate")) {
        decoded_body = decode_zlib(parsed.body);
      } else {
        return Result<HttpResponse>::failure(Error::make(
            "net.content_encoding_unsupported", "server sent '" + std::string(content_encoding) + "'"));
      }
      if (decoded_body.is_error()) return Result<HttpResponse>::failure(decoded_body.error());
      if (diagnostics != nullptr) {
        diagnostics->add(Severity::Hint, SourcePos{}, "net.decoded: content-encoding " + std::string(content_encoding));
      }
      parsed.body = std::move(decoded_body.value());
    }

    return Result<HttpResponse>::success(std::move(parsed));
  }
}

Result<HttpResponse> http_get(const Url& url, Diagnostics* diagnostics, int timeout_ms) {
  HttpRequest request;
  request.url = url;
  request.timeout_ms = timeout_ms;
  return http_request(request, diagnostics);
}

}  // namespace bonk::net
