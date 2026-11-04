#include "bonk/shell/shell.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include "bonk/html/html.hpp"
#include "bonk/net/http.hpp"
#include "bonk/net/url.hpp"

namespace bonk::shell {
namespace {

bool has_scheme(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  for (std::size_t i = 0; i < colon; ++i) {
    const char c = text[i];
    const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
    if (!valid) return false;
  }
  return true;
}

/// Splits "path/to/file.html" into ("path/to", "file.html").
std::pair<std::string, std::string> split_path(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) return {"", std::string(path)};
  return {std::string(path.substr(0, slash)), std::string(path.substr(slash + 1))};
}

std::string file_stem_title(std::string_view path) {
  const std::pair<std::string, std::string> parts = split_path(path);
  return parts.second.empty() ? "Untitled" : parts.second;
}

Status io_failure(std::string_view code, std::string message) {
  return Status::failure(Error::make(code, std::move(message)));
}

}  // namespace

Session::Session(float viewport_width, float viewport_height, std::string font_path)
    : viewport_width_(viewport_width), viewport_height_(viewport_height), font_path_(std::move(font_path)) {
  if (font_path_.empty()) {
    font_ = text::Font::system_default();
  } else {
    Result<text::Font> loaded = text::Font::load_file(font_path_);
    if (loaded.is_ok()) {
      font_ = std::move(loaded.value());
    } else {
      font_ = text::Font::system_default();
      tab_.error = loaded.error().to_string();
    }
  }
  font_name_ = std::string(font_.name());
}

Session::~Session() = default;

std::string Session::resolve_target(std::string_view target) const {
  if (has_scheme(target)) return std::string(target);
  if (target.starts_with("//")) return "https:" + std::string(target);

  // Page loaded from the network: resolve like a browser would.
  if (!tab_.base.empty() && has_scheme(tab_.base) && !tab_.base.starts_with("file:")) {
    const Result<net::Url> base = net::parse_url(tab_.base);
    if (base.is_ok()) {
      const Result<net::Url> resolved = net::parse_url(std::string(target), &base.value());
      if (resolved.is_ok()) return resolved.value().serialize();
    }
    return std::string(target);
  }

  // Local page: relative to the containing directory.
  if (!tab_.base.empty()) {
    if (target.starts_with('/')) return "file:" + std::string(target);
    std::string combined = tab_.base;
    if (!combined.empty() && combined.back() != '/') combined += '/';
    combined.append(target);
    return combined;
  }
  return std::string(target);
}

Status Session::navigate(std::string_view target, std::string_view base) {
  if (!base.empty()) {
    // Explicit base (e.g. <base href> or a link from a different document).
    const Result<net::Url> base_url = net::parse_url(std::string(base));
    if (base_url.is_ok()) {
      const Result<net::Url> resolved = net::parse_url(std::string(target), &base_url.value());
      if (resolved.is_ok()) return load(resolved.value().serialize(), "resolved");
    }
  }
  return load(target, {});
}

Status Session::load(std::string_view target, std::string_view /*base_for_relative*/) {
  const std::string resolved = resolve_target(target);

  std::string html_bytes;
  std::string base;
  std::string display_url = resolved;

  if (resolved.starts_with("http://") || resolved.starts_with("https://")) {
    const Result<net::Url> url = net::parse_url(resolved);
    if (url.is_error()) return Status::failure(url.error());
    if (url.value().scheme == "https") {
      return io_failure("net.tls_not_implemented", "HTTPS is not implemented yet (see docs/ROADMAP.md, stage 2b)");
    }
    Diagnostics diagnostics;
    Result<net::HttpResponse> response = net::http_get(url.value(), &diagnostics);
    if (response.is_error()) return Status::failure(response.error());
    if (response.value().status < 200 || response.value().status >= 300) {
      return io_failure("net.bad_status", "HTTP status " + std::to_string(response.value().status) + " for " + resolved);
    }
    html_bytes = std::move(response.value().body);
    base = response.value().final_url.empty() ? resolved : response.value().final_url;
    display_url = base;
  } else {
    std::string path = resolved;
    if (path.starts_with("file://")) {
      path = path.substr(7);
    } else if (path.starts_with("file:")) {
      path = path.substr(5);
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) return io_failure("io.open_failed", "cannot open '" + path + "'");
    html_bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    base = split_path(path).first;
    display_url = path;
  }

  auto document = std::make_unique<dom::Document>(base);
  Diagnostics diagnostics;
  Status parsed = html::parse_html(html_bytes, *document, &diagnostics);
  if (parsed.is_error()) return parsed;

  // Page CSS: <style> blocks plus local <link rel="stylesheet"> files.
  std::string css_text;
  for (const dom::NodeId node : document->get_elements_by_tag_name("style")) {
    css_text += document->text_content(node);
    css_text += "\n";
  }
  for (const dom::NodeId node : document->get_elements_by_tag_name("link")) {
    const std::string_view rel = document->attribute(node, "rel");
    if (rel.find("stylesheet") == std::string_view::npos) continue;
    const std::string_view href = document->attribute(node, "href");
    if (href.empty() || has_scheme(href)) continue;  // remote stylesheets need TLS/HTTP plumbing
    std::string path = base;
    if (!path.empty()) path += '/';
    path.append(href);
    std::ifstream style_file{path, std::ios::binary};
    if (!style_file) continue;
    css_text.append(std::istreambuf_iterator<char>(style_file), std::istreambuf_iterator<char>());
    css_text += "\n";
  }

  Result<css::StyleSheet> sheet = css::StyleSheet::parse(css_text, &diagnostics);
  if (sheet.is_error()) return Status::failure(sheet.error());

  // Images: local sources only; failures become placeholder boxes.
  image::ImageStore images;
  for (const dom::NodeId node : document->get_elements_by_tag_name("img")) {
    const std::string_view src = document->attribute(node, "src");
    if (src.empty() || has_scheme(src)) continue;
    std::string error;
    (void)images.add_from_file(src, base, error);
  }

  document_ = std::move(document);
  sheet_ = std::move(sheet.value());
  images_ = std::move(images);

  tab_.url = display_url;
  tab_.base = base;
  tab_.scroll_y = 0.0f;
  tab_.state = LoadState::Loading;
  tab_.error.clear();

  const Status laid_out = rebuild_layout();
  if (laid_out.is_error()) {
    tab_.state = LoadState::Failed;
    tab_.error = laid_out.error().to_string();
    return laid_out;
  }

  std::string title;
  for (const dom::NodeId node : document_->get_elements_by_tag_name("title")) {
    title = document_->text_content(node);
    if (!title.empty()) break;
  }
  tab_.title = title.empty() ? file_stem_title(display_url) : title;
  tab_.state = LoadState::Ready;

  if (tab_.history.empty() || tab_.history[tab_.history_index] != display_url) {
    tab_.history.resize(tab_.history_index + 1);
    tab_.history.push_back(display_url);
    tab_.history_index = tab_.history.size() - 1;
  }
  return Status::success();
}

Status Session::rebuild_layout() {
  if (document_ == nullptr) return io_failure("shell.no_document", "nothing to lay out");
  styles_ = css::compute_styles(*document_, sheet_, viewport_width_, nullptr);
  layout::LayoutViewport viewport{viewport_width_, viewport_height_};
  layout_ = std::make_unique<layout::LayoutResult>(
      layout::layout_document(*document_, styles_, font_, viewport, &images_));
  tab_.content_height = layout_->document_height;
  node_count_ = document_->node_count();
  return Status::success();
}

Status Session::back() {
  if (!can_go_back()) return io_failure("shell.no_history", "no previous page");
  --tab_.history_index;
  const std::string target = tab_.history[tab_.history_index];
  const std::size_t saved = tab_.history_index;
  const Status status = load(target, "history");
  // load() records a new entry only when the URL differs from history[index];
  // stepping through history therefore leaves the list (and index) untouched.
  tab_.history_index = std::min(saved, tab_.history.size() - 1);
  return status;
}

Status Session::forward() {
  if (!can_go_forward()) return io_failure("shell.no_history", "no next page");
  ++tab_.history_index;
  const std::string target = tab_.history[tab_.history_index];
  const std::size_t saved = tab_.history_index;
  const Status status = load(target, "history");
  tab_.history_index = std::min(saved, tab_.history.size() - 1);
  return status;
}

bool Session::can_go_back() const noexcept { return tab_.history_index > 0 && !tab_.history.empty(); }
bool Session::can_go_forward() const noexcept {
  return !tab_.history.empty() && tab_.history_index + 1 < tab_.history.size();
}

float Session::max_scroll() const noexcept {
  const float visible = viewport_height_;
  return std::max(0.0f, tab_.content_height - visible);
}

void Session::scroll_by(float delta) { set_scroll(tab_.scroll_y + delta); }

void Session::set_scroll(float y) {
  tab_.scroll_y = std::clamp(y, 0.0f, max_scroll());
}

ClickOutcome Session::click(float x, float y) {
  ClickOutcome outcome;
  if (layout_ == nullptr || document_ == nullptr) return outcome;

  const layout::Box* hit = layout::hit_test(layout_->root, x, y + tab_.scroll_y);
  if (hit == nullptr) return outcome;

  // Walk up the box tree (each box keeps its DOM node) looking for a link.
  dom::NodeId node = hit->node;
  while (node.valid() && document_->is_alive(node)) {
    if (document_->tag_name(node) == "a") {
      const std::string_view href = document_->attribute(node, "href");
      if (!href.empty()) {
        outcome.followed_link = true;
        outcome.href = resolve_target(href);
        const Status status = load(href, {});
        outcome.needs_reload = status.is_ok();
        if (status.is_error()) tab_.error = status.error().to_string();
        return outcome;
      }
    }
    node = document_->parent(node);
  }
  return outcome;
}

void Session::resize(float width, float height) {
  viewport_width_ = std::max(1.0f, width);
  viewport_height_ = std::max(1.0f, height);
  if (document_ != nullptr) {
    (void)rebuild_layout();
    set_scroll(tab_.scroll_y);
  }
}

void Session::reload() {
  if (tab_.url.empty()) return;
  const std::string url = tab_.url;
  (void)load(url, "reload");
}

bool Session::render(paint::Canvas& canvas) const {
  if (layout_ == nullptr) {
    canvas.clear(css::Color{255, 255, 255, 255});
    return false;
  }
  paint::RenderOptions options;
  options.scroll_y = tab_.scroll_y;
  paint::render_layout(canvas, layout_->root, font_, options);
  return true;
}

Status Session::render_to_png(std::string_view path) const {
  paint::Canvas canvas(static_cast<int>(viewport_width_), static_cast<int>(viewport_height_));
  (void)render(canvas);
  if (!canvas.write_png(path)) return io_failure("io.write_failed", "cannot write '" + std::string(path) + "'");
  return Status::success();
}

}  // namespace bonk::shell
