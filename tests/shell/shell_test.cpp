#include <fstream>
#include <string>

#include "bonk/shell/shell.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

std::string write_page(const std::string& name, std::string_view html) {
  const std::string path = "/tmp/bonk-shell-" + name;
  std::ofstream file(path, std::ios::binary);
  file << html;
  return path;
}

std::string tall_page() {
  std::string html = "<html><head><title>Tall</title><style>body{margin:0}div{height:400px}</style></head><body>";
  for (int i = 0; i < 5; ++i) html += "<div>block</div>";
  html += "</body></html>";
  return html;
}

}  // namespace

BONK_TEST(shell, navigates_a_local_file) {
  const std::string path = write_page("basic.html",
                                      "<html><head><title>Hello title</title><style>body{margin:0}</style></head>"
                                      "<body><h1>Hi</h1><p>text</p></body></html>");
  shell::Session session(800.0f, 600.0f);
  const Status status = session.navigate(path);
  REQUIRE(status.is_ok());
  CHECK_EQ(session.tab().title, std::string("Hello title"));
  CHECK(session.tab().state == shell::LoadState::Ready);
  CHECK(session.node_count() > 5);
  CHECK_EQ(session.scroll_y(), 0.0f);
}

BONK_TEST(shell, scroll_is_clamped_to_content) {
  const std::string path = write_page("tall.html", tall_page());
  shell::Session session(640.0f, 400.0f);
  REQUIRE(session.navigate(path).is_ok());
  CHECK(session.tab().content_height >= 2000.0f);

  session.scroll_by(100000.0f);
  CHECK_EQ(session.scroll_y(), session.max_scroll());
  session.scroll_by(-100000.0f);
  CHECK_EQ(session.scroll_y(), 0.0f);
}

BONK_TEST(shell, clicking_a_link_navigates) {
  const std::string target = write_page("target.html",
                                        "<html><head><title>Target</title><style>body{margin:0}</style></head>"
                                        "<body><p>target page</p></body></html>");
  (void)target;
  const std::string link =
      write_page("link.html", "<html><head><style>body{margin:0}a{display:block;height:120px}</style></head>"
                              "<body><a href=\"bonk-shell-target.html\">go</a><p>after</p></body></html>");

  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(link).is_ok());

  const shell::ClickOutcome outcome = session.click(20.0f, 20.0f);
  CHECK(outcome.followed_link);
  CHECK(outcome.needs_reload);
  CHECK(outcome.href.find("bonk-shell-target.html") != std::string::npos);
  CHECK_EQ(session.tab().title, std::string("Target"));
}

BONK_TEST(shell, clicking_plain_text_does_nothing) {
  const std::string path = write_page("plain.html",
                                      "<html><head><style>body{margin:0}</style></head><body><p>just text</p></body></html>");
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(path).is_ok());
  const shell::ClickOutcome outcome = session.click(10.0f, 10.0f);
  CHECK(!outcome.followed_link);
}

BONK_TEST(shell, history_back_and_forward) {
  const std::string first = write_page("a.html", "<html><body><title>A</title>a</body></html>");
  const std::string second = write_page("b.html", "<html><body><title>B</title>b</body></html>");
  shell::Session session(400.0f, 300.0f);
  REQUIRE(session.navigate(first).is_ok());
  REQUIRE(session.navigate(second).is_ok());
  CHECK_EQ(session.tab().title, std::string("B"));
  CHECK(session.can_go_back());

  REQUIRE(session.back().is_ok());
  CHECK_EQ(session.tab().title, std::string("A"));
  CHECK(session.can_go_forward());

  REQUIRE(session.forward().is_ok());
  CHECK_EQ(session.tab().title, std::string("B"));
}

BONK_TEST(shell, missing_file_reports_an_error) {
  shell::Session session(400.0f, 300.0f);
  const Status status = session.navigate("/tmp/definitely-not-a-bonk-page.html");
  REQUIRE(status.is_error());
  CHECK_EQ(status.error().code, std::string("io.open_failed"));
}

BONK_TEST(shell, https_uses_the_tls_stack) {
  // Either the network works and the page loads, or the failure comes from the TLS
  // transport — never from a "not implemented" stub.
  shell::Session session(400.0f, 300.0f);
  const Status status = session.navigate("https://127.0.0.1:1/");
  REQUIRE(status.is_error());
  CHECK(status.error().code != std::string("net.tls_not_implemented"));
  CHECK(status.error().code.starts_with("net."));
}

BONK_TEST(shell, renders_a_png_for_the_viewport) {
  const std::string path = write_page("shot.html", tall_page());
  shell::Session session(320.0f, 200.0f);
  REQUIRE(session.navigate(path).is_ok());
  session.set_scroll(120.0f);
  REQUIRE(session.render_to_png("/tmp/bonk-shell-shot.png").is_ok());

  std::ifstream file{"/tmp/bonk-shell-shot.png", std::ios::binary};
  REQUIRE(static_cast<bool>(file));
  const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  CHECK(bytes.size() > 100);
  CHECK_EQ(bytes.substr(1, 3), std::string("PNG"));
}
