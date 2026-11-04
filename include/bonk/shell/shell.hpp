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

#include "bonk/core/result.hpp"
#include "bonk/css/css.hpp"
#include "bonk/dom/dom.hpp"
#include "bonk/image/image.hpp"
#include "bonk/layout/layout.hpp"
#include "bonk/paint/canvas.hpp"
#include "bonk/paint/render.hpp"
#include "bonk/text/font.hpp"

namespace bonk::shell {

enum class LoadState { Empty, Loading, Ready, Failed };

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

  std::vector<std::string> history;
  std::size_t history_index = 0;
};

/// One tab's worth of state plus the engine objects for the page it shows.
class Session {
 public:
  explicit Session(float viewport_width, float viewport_height, std::string font_path = {});
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

 private:
  Status load(std::string_view target, std::string_view base_for_relative);
  Status rebuild_layout();
  [[nodiscard]] std::string resolve_target(std::string_view target) const;

  float viewport_width_ = 1024.0f;
  float viewport_height_ = 768.0f;
  std::string font_path_;
  std::string font_name_;

  text::Font font_;
  Tab tab_;
  std::unique_ptr<dom::Document> document_;
  css::StyleSheet sheet_;
  css::StyleMap styles_;
  image::ImageStore images_;
  std::unique_ptr<layout::LayoutResult> layout_;
  std::size_t node_count_ = 0;
};

}  // namespace bonk::shell
