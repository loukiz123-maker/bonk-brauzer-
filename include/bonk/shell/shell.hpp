// Bonk Brauzer — the browser shell: a tab, navigation, history, scrolling and clicks.
//
// The shell owns no windowing code. It turns user intents (navigate / back / scroll /
// click) into engine calls and produces pixels on demand, so the same logic drives the
// X11 window (src/shell/x11.cpp) and the headless tests.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/bindings/bindings.hpp"
#include "bonk/core/diag.hpp"
#include "bonk/core/result.hpp"
#include "bonk/css/css.hpp"
#include "bonk/dom/dom.hpp"
#include "bonk/image/image.hpp"
#include "bonk/js/js.hpp"
#include "bonk/layout/layout.hpp"
#include "bonk/net/cookie.hpp"
#include "bonk/net/http.hpp"
#include "bonk/paint/canvas.hpp"
#include "bonk/paint/render.hpp"
#include "bonk/security/security.hpp"
#include "bonk/text/font.hpp"

namespace bonk::shell {

enum class LoadState { Empty, Loading, Ready, Failed };

/// One entry of the devtools network panel.
struct NetworkEntry {
  std::string method;
  std::string url;
  int status = 0;
  std::size_t request_bytes = 0;
  std::size_t response_bytes = 0;
  double duration_ms = 0.0;
  std::string blocked;  // "cors.*" / "csp.*" when the request never completed
};

/// Everything the devtools panel shows for a page.
struct DevToolsSnapshot {
  std::string url;
  std::string title;
  security::SecurityState security = security::SecurityState::None;
  std::string csp;
  std::string origin;
  std::size_t node_count = 0;
  std::size_t scripts_run = 0;
  std::size_t scripts_blocked = 0;
  std::vector<bindings::ScriptRun> scripts;
  std::vector<js::ConsoleEntry> console;
  std::vector<NetworkEntry> network;
};

struct ClickOutcome {
  bool followed_link = false;
  std::string href;    // resolved absolute target when a link was hit
  bool needs_reload = false;
};

struct Tab {
  std::string url;      // what the address bar shows
  std::string base;     // directory (file:) or URL used to resolve relative links
  std::string title;
  LoadState state = LoadState::Empty;
  std::string error;    // last failure, human readable
  float scroll_y = 0.0f;
  float content_height = 0.0f;
  security::SecurityState security = security::SecurityState::None;
  std::string origin;  // serialised origin ("null" for local files)

  std::vector<std::string> history;
  std::size_t history_index = 0;
};

/// One tab's worth of state plus the engine objects for the page it shows.
class Session {
 public:
  /// `profile_dir` enables persistent storage (cookies live in memory only).
  explicit Session(float viewport_width, float viewport_height, std::string font_path = {},
                   std::string profile_dir = {});
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;

  /// Accepts `file:`, `http:`, `https:` URLs and bare paths. Relative targets are
  /// resolved against `base` (or the current page).
  Status navigate(std::string_view target, std::string_view base = {});
  Status back();
  Status forward();
  [[nodiscard]] bool can_go_back() const noexcept;
  [[nodiscard]] bool can_go_forward() const noexcept;

  void scroll_by(float delta);
  void set_scroll(float y);
  [[nodiscard]] float scroll_y() const noexcept { return tab_.scroll_y; }

  ClickOutcome click(float x, float y);

  void resize(float width, float height);
  void reload();

  /// Paints the current scroll position into `canvas` (viewport-sized).
  bool render(paint::Canvas& canvas) const;
  /// Convenience for the CLI/tests: paints and writes a PNG.
  Status render_to_png(std::string_view path) const;

  [[nodiscard]] const Tab& tab() const noexcept { return tab_; }
  [[nodiscard]] float viewport_width() const noexcept { return viewport_width_; }
  [[nodiscard]] float viewport_height() const noexcept { return viewport_height_; }
  [[nodiscard]] const std::string& font_name() const noexcept { return font_name_; }
  [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }

  /// Maximum scroll offset that still shows content (0 when the page fits).
  [[nodiscard]] float max_scroll() const noexcept;

  /// The page's DOM / script bindings (null before the first load).
  [[nodiscard]] const dom::Document* document() const noexcept { return document_.get(); }
  [[nodiscard]] bindings::Bindings* bindings() noexcept { return bindings_.get(); }

  /// First box laid out for `node` (nullptr when the node is not rendered).
  [[nodiscard]] const layout::Box* box_of(dom::NodeId node) const;

  /// Computed styles of the current page (for the devtools inspector).
  [[nodiscard]] const css::StyleMap& styles() const noexcept { return styles_; }

  /// Devtools view of the current page (console, network, CSP, security state).
  [[nodiscard]] DevToolsSnapshot devtools() const;

  /// Cookies for the current browsing session (shared by every page in the tab).
  [[nodiscard]] net::CookieJar& cookies() noexcept { return cookies_; }

  /// JavaScript side of the loaded page (empty until a page is loaded).
  [[nodiscard]] const std::vector<bindings::ScriptRun>& script_runs() const noexcept;
  [[nodiscard]] const std::vector<js::ConsoleEntry>& js_console() const noexcept;
  [[nodiscard]] bool dispatch_event(dom::NodeId target, std::string_view type);

 private:
  Status load(std::string_view target, std::string_view base_for_relative);
  /// Tail of `load`: builds document/CSS/JS objects for already-fetched HTML.
  Status load_html(std::string html_bytes, std::string base, std::string display_url,
                   const std::vector<std::pair<std::string, std::string>>& response_headers = {});
  /// Transport for `XMLHttpRequest`: CORS/CSP checks, cookies, devtools logging.
  bindings::NetworkResponse perform_xhr(const bindings::NetworkRequest& request);
  /// Blocks a subresource (`img`, `link`, `script`) that policy forbids.
  bool allow_subresource(std::string_view kind, std::string_view url, Diagnostics* diagnostics);
  /// Sends a form to the network (POST) and renders the reply.
  Status load_posted(const bindings::FormSubmission& submission, const std::string& action);
  /// Fires `submit`, then navigates with GET (query string) or POST.
  Status submit_form(const bindings::FormSubmission& submission);
  /// Drains submissions queued by scripts; true when one was submitted.
  bool drain_pending_submissions();
  Status rebuild_layout();
  /// Re-reads `<title>` into the tab (scripts may change it after load).
  void refresh_title();
  [[nodiscard]] std::string resolve_target(std::string_view target) const;

  float viewport_width_ = 1024.0f;
  float viewport_height_ = 768.0f;
  std::string font_path_;
  std::string font_name_;
  std::string profile_dir_;
  net::CookieJar cookies_;

  text::Font font_;
  Tab tab_;
  std::unique_ptr<dom::Document> document_;
  css::StyleSheet sheet_;
  css::StyleMap styles_;
  image::ImageStore images_;
  std::unique_ptr<layout::LayoutResult> layout_;
  std::unique_ptr<js::Context> js_context_;
  std::unique_ptr<bindings::Bindings> bindings_;
  std::vector<layout::CanvasBitmap> canvas_bitmaps_;
  std::size_t node_count_ = 0;
  std::vector<NetworkEntry> network_log_;
  std::size_t scripts_blocked_ = 0;
};

}  // namespace bonk::shell
