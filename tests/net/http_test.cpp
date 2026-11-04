#include <atomic>
#include <string>
#include <thread>

#include "test_framework.hpp"

#include "bonk/net/http.hpp"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace bonk;
using namespace bonk::net;

namespace {

#if !defined(_WIN32)

/// Minimal one-shot TCP server: accepts a single connection, sends `response`,
/// then closes. Runs on its own thread so the client sees a real socket.
class OneShotServer {
 public:
  explicit OneShotServer(std::string response, int connection_count = 1) : response_(std::move(response)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int enable = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;  // ephemeral
    ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    ::listen(listener_, 4);
    thread_ = std::thread([this, connection_count]() { serve(connection_count); });
  }

  ~OneShotServer() {
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) ::close(listener_);
  }

  [[nodiscard]] int port() const { return port_; }

 private:
  void serve(int connections) {
    for (int i = 0; i < connections; ++i) {
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) return;
      char buffer[4096];
      ::recv(client, buffer, sizeof(buffer), 0);  // request is irrelevant for the test
      ::send(client, response_.data(), response_.size(), 0);
      ::close(client);
    }
  }

  std::string response_;
  int listener_ = -1;
  int port_ = 0;
  std::thread thread_;
};

#endif

}  // namespace

BONK_TEST(http, chunked_decoding) {
  const Result<std::string> decoded = decode_chunked("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
  REQUIRE(decoded.is_ok());
  CHECK_EQ(decoded.value(), std::string("Wikipedia"));

  CHECK(decode_chunked("4\r\nWiki\r\n").is_error());           // truncated
  CHECK(decode_chunked("zz\r\nabc\r\n").is_error());           // bad size
  const Result<std::string> extended = decode_chunked("3;ext=1\r\nabc\r\n0\r\n\r\n");
  REQUIRE(extended.is_ok());
  CHECK_EQ(extended.value(), std::string("abc"));
}

BONK_TEST(http, rejects_unsupported_schemes) {
  const Result<Url> https_url = parse_url("https://127.0.0.1:1/");  // closed port: offline-safe
  REQUIRE(https_url.is_ok());
  const Result<HttpResponse> secure = http_get(https_url.value());
  REQUIRE(secure.is_error());
  // https is supported now; the failure must come from the transport, not from a
  // "not implemented" stub. Port 1 is closed, so the connect fails.
  CHECK(secure.error().code != std::string("net.tls_not_implemented"));

  const Result<Url> ftp_url = parse_url("ftp://example.com/x");
  REQUIRE(ftp_url.is_ok());
  const Result<HttpResponse> ftp = http_get(ftp_url.value());
  REQUIRE(ftp.is_error());
  CHECK_EQ(ftp.error().code, std::string("net.unsupported_scheme"));
}

#if !defined(_WIN32)

BONK_TEST(http, get_with_content_length) {
  const std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html\r\n"
      "Content-Length: 14\r\n"
      "\r\n"
      "<h1>hello</h1>";
  OneShotServer server(response);

  const Result<Url> url = parse_url("http://127.0.0.1:" + std::to_string(server.port()) + "/page");
  REQUIRE(url.is_ok());
  const Result<HttpResponse> result = http_get(url.value());
  REQUIRE(result.is_ok());
  CHECK_EQ(result.value().status, 200);
  CHECK_EQ(result.value().reason, std::string("OK"));
  CHECK_EQ(result.value().header("content-type"), std::string_view("text/html"));
  CHECK_EQ(result.value().body, std::string("<h1>hello</h1>"));
  CHECK_EQ(result.value().final_url, std::string("http://127.0.0.1:" + std::to_string(server.port()) + "/page"));
}

BONK_TEST(http, chunked_transfer_encoding_over_the_wire) {
  const std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nBonk!\r\n5\r\nHello\r\n0\r\n\r\n";
  OneShotServer server(response);

  const Result<Url> url = parse_url("http://127.0.0.1:" + std::to_string(server.port()) + "/");
  REQUIRE(url.is_ok());
  const Result<HttpResponse> result = http_get(url.value());
  REQUIRE(result.is_ok());
  CHECK_EQ(result.value().body, std::string("Bonk!Hello"));
}

BONK_TEST(http, follows_redirects) {
  // Target server answers 200; the redirector answers 302 pointing at it.
  OneShotServer target("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal", 1);
  const std::string redirect = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" +
                               std::to_string(target.port()) + "/final\r\nContent-Length: 0\r\n\r\n";
  OneShotServer redirector(redirect, 1);

  const Result<Url> url = parse_url("http://127.0.0.1:" + std::to_string(redirector.port()) + "/start");
  REQUIRE(url.is_ok());
  const Result<HttpResponse> result = http_get(url.value());
  REQUIRE(result.is_ok());
  CHECK_EQ(result.value().status, 200);
  CHECK_EQ(result.value().body, std::string("final"));
  CHECK_EQ(result.value().redirect_count, 1);
}

BONK_TEST(http, connection_refused_is_an_error) {
  // Port 1 is almost never open; a refused connection must be a clean error.
  const Result<Url> url = parse_url("http://127.0.0.1:1/");
  REQUIRE(url.is_ok());
  const Result<HttpResponse> result = http_get(url.value(), nullptr, 300);
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("net.connect_failed"));
}

BONK_TEST(http, dns_failure_is_an_error) {
  const Result<Url> url = parse_url("http://host.invalid.tld/");
  REQUIRE(url.is_ok());
  const Result<HttpResponse> result = http_get(url.value(), nullptr, 500);
  REQUIRE(result.is_error());
  CHECK(result.error().code == "net.dns_failure" || result.error().code == "net.connect_failed");
}

#else

BONK_TEST(http, sockets_are_unimplemented_on_windows) {
  CHECK(true);  // documented limitation: the socket layer is POSIX-only for now
}

#endif
