// Shell-level security behaviour: CSP on inline scripts, same-origin and
// cross-origin XMLHttpRequest (CORS), and the devtools network log.
//
// The HTTP server below is a loopback socket owned by the test process, so the
// checks are deterministic and need no internet access.
#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "bonk/shell/shell.hpp"
#include "test_framework.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define BONK_TEST_HAS_SOCKETS 1
#else
#define BONK_TEST_HAS_SOCKETS 0
#endif

using namespace bonk;

#if BONK_TEST_HAS_SOCKETS
namespace {

/// Minimal HTTP/1.1 server: replies to the next N connections with the queued
/// responses (in order), then stops. One request per connection.
class LocalServer {
 public:
  explicit LocalServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return;
    socklen_t length = sizeof(address);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) return;
    port_ = ntohs(address.sin_port);
    if (::listen(listen_fd_, 8) != 0) return;
    thread_ = std::thread([this]() { run(); });
  }

  ~LocalServer() {
    running_ = false;
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) thread_.join();
  }

  LocalServer(const LocalServer&) = delete;
  LocalServer& operator=(const LocalServer&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  [[nodiscard]] std::string url(std::string_view path = "/") const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

 private:
  void run() {
    for (std::size_t index = 0; index < responses_.size() && running_; ++index) {
      const int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) return;
      char buffer[4096];
      // Read the request head (we do not need the body for these tests).
      std::string received;
      while (received.find("\r\n\r\n") == std::string::npos) {
        const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        received.append(buffer, static_cast<std::size_t>(got));
      }
      const std::string& response = responses_[index];
      std::size_t sent = 0;
      while (sent < response.size()) {
        const ssize_t written = ::send(client, response.data() + sent, response.size() - sent, 0);
        if (written <= 0) break;
        sent += static_cast<std::size_t>(written);
      }
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }

  std::vector<std::string> responses_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

std::string http_response(std::string_view body, std::string_view extra_headers = {}) {
  std::string out = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n";
  out += "Content-Length: ";
  out += std::to_string(body.size());
  out += "\r\n";
  out += extra_headers;
  out += "Connection: close\r\n\r\n";
  out += body;
  return out;
}

std::string page_with_script(const std::string& script, std::string_view head_extra = {}) {
  std::string html = "<html><head><title>page</title>";
  html += head_extra;
  html += "</head><body><p>hello</p><script>";
  html += script;
  html += "</script></body></html>";
  return html;
}

}  // namespace

BONK_TEST(security_shell, csp_meta_blocks_inline_scripts) {
  LocalServer server({http_response(page_with_script(
      "document.title = 'script ran';",
      "<meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'self'\">"))});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server.url("/")).is_ok());
  CHECK_EQ(session.tab().title, std::string("page"));  // the script never ran
  const shell::DevToolsSnapshot tools = session.devtools();
  CHECK_EQ(tools.scripts_blocked, std::size_t{1});
  CHECK_EQ(tools.scripts_run, std::size_t{0});
  CHECK(tools.csp.find("script-src") != std::string::npos);
  REQUIRE(tools.scripts.size() == 1);
  CHECK(tools.scripts[0].message.find("Content-Security-Policy") != std::string::npos);
}

BONK_TEST(security_shell, csp_header_from_http_response_is_applied) {
  LocalServer server({http_response(page_with_script("document.title = 'ran';"),
                                    "Content-Security-Policy: script-src 'none'\r\n")});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server.url("/")).is_ok());
  CHECK_EQ(session.tab().title, std::string("page"));
  CHECK_EQ(session.devtools().scripts_blocked, std::size_t{1});
}

BONK_TEST(security_shell, same_origin_xhr_reads_the_body) {
  const std::string script =
      "var xhr = new XMLHttpRequest();"
      "xhr.open('GET', '/data', false);"
      "xhr.send();"
      "document.title = xhr.status + ':' + xhr.responseText;";
  LocalServer server({http_response(page_with_script(script)), http_response("payload")});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server.url("/")).is_ok());
  CHECK_EQ(session.tab().title, std::string("200:payload"));
  const shell::DevToolsSnapshot tools = session.devtools();
  REQUIRE(tools.network.size() == 1);
  CHECK_EQ(tools.network[0].status, 200);
  CHECK(tools.network[0].blocked.empty());
  CHECK_EQ(tools.network[0].response_bytes, std::size_t{7});
}

BONK_TEST(security_shell, cross_origin_xhr_without_cors_is_blocked) {
  // Page on server A, data on server B (different port = different origin).
  const std::string body = http_response("secret");
  LocalServer server_b({body});
  const std::string script =
      "var xhr = new XMLHttpRequest();"
      "xhr.open('GET', '" + server_b.url("/secret") + "', false);"
      "xhr.send();"
      "document.title = xhr.status + ':' + xhr.error;";
  LocalServer server_a({http_response(page_with_script(script))});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server_a.url("/")).is_ok());
  CHECK_EQ(session.tab().title, std::string("0:cors.allow_origin_missing"));
  const shell::DevToolsSnapshot tools = session.devtools();
  REQUIRE(tools.network.size() == 1);
  CHECK_EQ(tools.network[0].blocked, std::string("cors.allow_origin_missing"));
}

BONK_TEST(security_shell, cross_origin_xhr_with_cors_header_succeeds) {
  LocalServer server_b({http_response("shared", "Access-Control-Allow-Origin: *\r\n")});
  const std::string script =
      "var xhr = new XMLHttpRequest();"
      "xhr.open('GET', '" + server_b.url("/shared") + "', false);"
      "xhr.send();"
      "document.title = xhr.status + ':' + xhr.responseText;";
  LocalServer server_a({http_response(page_with_script(script))});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server_a.url("/")).is_ok());
  CHECK_EQ(session.tab().title, std::string("200:shared"));
  // Cross-origin XHR is not credentialed, so a wildcard allow-origin is enough.
  CHECK(session.devtools().network[0].blocked.empty());
}

BONK_TEST(security_shell, cors_denied_preflight_for_custom_headers) {
  const std::string script =
      "var xhr = new XMLHttpRequest();"
      "xhr.open('GET', 'http://127.0.0.1:1/api', false);"
      "xhr.setRequestHeader('X-Api-Key', 'k');"
      "xhr.send();"
      "document.title = xhr.status + ':' + xhr.error;";
  LocalServer server({http_response(page_with_script(script))});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server.url("/")).is_ok());
  CHECK_EQ(session.tab().title, std::string("0:cors.preflight_required_header"));
}

BONK_TEST(security_shell, secure_pages_report_their_security_state) {
  LocalServer server({http_response(page_with_script("document.title = 'plain';"))});
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(server.url("/")).is_ok());
  CHECK(session.tab().security == security::SecurityState::Insecure);
  CHECK_EQ(session.tab().origin, server.url("/").substr(0, server.url("/").size() - 1));
  CHECK_EQ(std::string(security::to_string(session.devtools().security)), std::string("insecure"));
}

BONK_TEST(security_shell, file_pages_have_an_opaque_origin) {
  const std::string path = "/tmp/bonk-security-opaque.html";
  {
    std::ofstream file(path, std::ios::binary);
    file << "<html><body>local</body></html>";
  }
  shell::Session session(400.0f, 300.0f);
  REQUIRE(session.navigate(path).is_ok());
  CHECK(session.tab().security == security::SecurityState::None);
  CHECK_EQ(session.tab().origin, std::string("null"));
}

#endif  // BONK_TEST_HAS_SOCKETS
