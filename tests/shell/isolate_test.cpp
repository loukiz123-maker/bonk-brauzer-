// Process-per-tab isolation: the renderer runs in a child process that talks to
// the browser over a length-prefixed pipe protocol. A crashing renderer must be
// reported, not fatal.
#include <fstream>
#include <string>

#include "bonk/shell/isolate.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

std::string write_page(const std::string& name, std::string_view html) {
  const std::string path = "/tmp/bonk-isolate-" + name;
  std::ofstream file(path, std::ios::binary);
  file << html;
  return path;
}

}  // namespace

BONK_TEST(isolation, renderer_process_returns_a_frame) {
  if (!shell::isolation_supported()) return;  // documented fallback on non-POSIX builds
  const std::string path = write_page(
      "frame.html",
      "<html><head><title>Isolated</title><style>body{margin:0;background:#123456}\n"
      "h1{color:#ffffff;margin:16px}</style></head><body><h1>Child process</h1></body></html>");
  shell::IsolatedRequest request;
  request.target = path;
  request.width = 320;
  request.height = 200;
  const shell::IsolatedResult result = shell::render_isolated(request);
  REQUIRE(!result.crashed);
  REQUIRE(result.ok);
  CHECK_EQ(result.title, std::string("Isolated"));
  CHECK(result.png.size() > 1000);
  // PNG signature survived the IPC round trip.
  CHECK_EQ(result.png[0], std::uint8_t{0x89});
  CHECK_EQ(result.png[1], std::uint8_t{'P'});
  CHECK_EQ(result.png[2], std::uint8_t{'N'});
  CHECK(result.duration_ms >= 0.0);
}

BONK_TEST(isolation, missing_page_is_reported_by_the_child) {
  if (!shell::isolation_supported()) return;
  shell::IsolatedRequest request;
  request.target = "/tmp/bonk-isolate-definitely-missing.html";
  request.width = 200;
  request.height = 150;
  const shell::IsolatedResult result = shell::render_isolated(request);
  CHECK(!result.ok);
  CHECK(!result.crashed);  // a clean error, not a dead process
  CHECK(result.error.find("io.open_failed") != std::string::npos);
}

BONK_TEST(isolation, crashed_tab_does_not_kill_the_browser) {
  if (!shell::isolation_supported()) return;
  const std::string path = write_page("crash.html", "<html><body>crash me</body></html>");
  shell::IsolatedRequest request;
  request.target = path;
  request.width = 200;
  request.height = 150;
  request.simulate_crash = true;
  const shell::IsolatedResult result = shell::render_isolated(request);
  CHECK(result.crashed);
  CHECK(!result.ok);
  CHECK(result.png.empty());
  CHECK(result.error.find("shell.tab_crashed") != std::string::npos);

  // The browser (this process) is still able to render another tab.
  request.simulate_crash = false;
  const shell::IsolatedResult recovered = shell::render_isolated(request);
  CHECK(recovered.ok);
}
