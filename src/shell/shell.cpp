#include "bonk/shell/shell.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "bonk/core/strings.hpp"
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

Session::Session(float viewport_width, float viewport_height, std::string font_path, std::string profile_dir)
    : viewport_width_(viewport_width),
      viewport_height_(viewport_height),
      font_path_(std::move(font_path)),
      profile_dir_(std::move(profile_dir)) {
  if (!profile_dir_.empty()) {
    std::error_code ignored;
    std::filesystem::create_directories(profile_dir_, ignored);
  }
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
  std::vector<std::pair<std::string, std::string>> response_headers;

  if (resolved.starts_with("http://") || resolved.starts_with("https://")) {
    const Result<net::Url> url = net::parse_url(resolved);
    if (url.is_error()) return Status::failure(url.error());
    Diagnostics diagnostics;
    net::HttpRequest request;
    request.url = url.value();
    request.cookie_jar = &cookies_;
    Result<net::HttpResponse> response = net::http_request(request, &diagnostics);
    if (response.is_error()) return Status::failure(response.error());
    if (response.value().status < 200 || response.value().status >= 300) {
      return io_failure("net.bad_status", "HTTP status " + std::to_string(response.value().status) + " for " + resolved);
    }
    response_headers = response.value().headers;
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
    // A query string/fragment is not part of a file name, but it *is* part of
    // the address the user sees (`form.html?q=1`), so keep it for display.
    std::string query;
    if (const std::size_t separator = path.find_first_of("?#"); separator != std::string::npos) {
      query = path.substr(separator);
      path.resize(separator);
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) return io_failure("io.open_failed", "cannot open '" + path + "'");
    html_bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    base = split_path(path).first;
    display_url = (query.empty() || query.front() == '#') ? path : path + query;
  }
  return load_html(std::move(html_bytes), std::move(base), std::move(display_url), response_headers);
}

Status Session::load_html(std::string html_bytes, std::string base, std::string display_url,
                           const std::vector<std::pair<std::string, std::string>>& response_headers) {
  auto document = std::make_unique<dom::Document>(base);
  Diagnostics diagnostics;
  Status parsed = html::parse_html(html_bytes, *document, &diagnostics);
  if (parsed.is_error()) return parsed;

  // --- security context of the new document --------------------------------
  network_log_.clear();
  scripts_blocked_ = 0;
  security::Origin origin;
  security::SecurityState state = security::SecurityState::None;
  const Result<net::Url> page_url = net::parse_url(display_url);
  if (page_url.is_ok()) {
    origin = security::Origin::from_url(page_url.value());
    state = security::security_state(page_url.value());
  }
  tab_.origin = origin.serialize();
  tab_.security = state;

  security::CspPolicy csp;
  bool have_csp = false;
  for (const auto& [name, value] : response_headers) {
    if (!::bonk::equals_ci(name, "content-security-policy")) continue;
    csp = security::CspPolicy::parse(value);
    have_csp = true;
  }
  for (const dom::NodeId node : document->get_elements_by_tag_name("meta")) {
    if (!::bonk::equals_ci(document->attribute(node, "http-equiv"), "content-security-policy")) continue;
    const std::string content(document->attribute(node, "content"));
    if (content.empty()) continue;
    // Several policies may be present; merging keeps the strictest union.
    csp = security::CspPolicy::parse(content);
    have_csp = true;
  }
  (void)have_csp;

  // Page CSS is collected *after* scripts run, so a script that inserts a
  // <style> element or changes a class is reflected in the same paint.
  document_ = std::move(document);

  js_context_ = std::make_unique<js::Context>();
  bindings_ = std::make_unique<bindings::Bindings>(*js_context_, *document_);
  bindings_->set_font(&font_);
  bindings_->set_cookie_jar(&cookies_);
  bindings_->set_csp(csp);
  bindings_->set_network_handler(
      [this](const bindings::NetworkRequest& request) { return perform_xhr(request); });
  bindings_->local_storage().set_path(std::string());
  // Storage is per origin: one file per (scheme, host, port) keeps two sites
  // from reading each other's keys even before a full origin sandbox exists.
  // Local files are opaque, so they share a namespace per directory instead —
  // which is what makes `file://` demos and tests behave predictably.
  std::string storage_key = origin.serialize();
  if (origin.opaque) {
    storage_key = base.starts_with("/") ? "file:" + base : std::string();
  }
  if (!profile_dir_.empty() && !storage_key.empty()) {
    std::string storage_path = profile_dir_;
    if (!storage_path.empty() && storage_path.back() == '/') storage_path.pop_back();
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a
    for (const unsigned char c : storage_key) {
      hash ^= c;
      hash *= 1099511628211ull;
    }
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "/local-%016llx.txt", static_cast<unsigned long long>(hash));
    storage_path += suffix;
    bindings_->local_storage().set_path(storage_path);
    (void)bindings_->local_storage().load();
  }
  bindings_->install();
  bindings_->run_scripts();
  for (const bindings::ScriptRun& run : bindings_->script_runs()) {
    if (run.message.find("Content-Security-Policy") != std::string::npos) ++scripts_blocked_;
  }
  (void)bindings_->dispatch(document_->root(), "DOMContentLoaded");
  (void)bindings_->dispatch(document_->root(), "load");

  std::string css_text;
  for (const dom::NodeId node : document_->get_elements_by_tag_name("style")) {
    css_text += document_->text_content(node);
    css_text += "\n";
  }
  for (const dom::NodeId node : document_->get_elements_by_tag_name("link")) {
    const std::string_view rel = document_->attribute(node, "rel");
    if (rel.find("stylesheet") == std::string_view::npos) continue;
    const std::string_view href = document_->attribute(node, "href");
    if (href.empty()) continue;
    const std::string resolved = resolve_target(href);
    if (has_scheme(href) && !allow_subresource("style", resolved, &diagnostics)) continue;
    if (has_scheme(href)) continue;  // remote stylesheets need a fetch loop
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
  for (const dom::NodeId node : document_->get_elements_by_tag_name("img")) {
    const std::string_view src = document_->attribute(node, "src");
    if (src.empty()) continue;
    if (has_scheme(src)) {
      (void)allow_subresource("image", resolve_target(src), &diagnostics);
      continue;
    }
    std::string error;
    (void)images.add_from_file(src, base, error);
  }

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

void Session::refresh_title() {
  if (document_ == nullptr) return;
  for (const dom::NodeId node : document_->get_elements_by_tag_name("title")) {
    const std::string text = document_->text_content(node);
    if (!text.empty()) {
      tab_.title = text;
      return;
    }
  }
}

Status Session::rebuild_layout() {
  if (document_ == nullptr) return io_failure("shell.no_document", "nothing to lay out");
  // Control values (input/textarea/select) live in attributes; copy them into
  // text nodes so the layout has something to draw inside the control's box.
  if (bindings_ != nullptr) bindings_->materialize_form_controls();
  styles_ = css::compute_styles(*document_, sheet_, viewport_width_, nullptr);

  // Canvas bitmaps come from the bindings layer; rebuild them before every layout
  // so `ctx.fillRect(...)` in a script or an event handler reaches the screen.
  canvas_bitmaps_.clear();
  if (bindings_ != nullptr) {
    bindings_->refresh_canvases();
    for (const bindings::CanvasImage& image : bindings_->canvas_images()) {
      canvas_bitmaps_.push_back(layout::CanvasBitmap{image.node, &image.image});
    }
  }

  layout::LayoutViewport viewport{viewport_width_, viewport_height_};
  viewport.canvases = canvas_bitmaps_.empty() ? nullptr : &canvas_bitmaps_;
  layout_ = std::make_unique<layout::LayoutResult>(
      layout::layout_document(*document_, styles_, font_, viewport, &images_));
  tab_.content_height = layout_->document_height;
  node_count_ = document_->node_count();
  refresh_title();
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


// ------------------------------------------------------------------ forms -----

Status Session::load_posted(const bindings::FormSubmission& submission, const std::string& action) {
  const bool network = action.starts_with("http://") || action.starts_with("https://");
  if (!network) {
    // Local pages have no server: fall back to a GET so the query is at least visible.
    return load(action + (action.find('?') == std::string::npos ? "?" : "&") + submission.body, "form");
  }
  const Result<net::Url> url = net::parse_url(action);
  if (url.is_error()) return Status::failure(url.error());

  Diagnostics diagnostics;
  net::HttpRequest request;
  request.url = url.value();
  request.method = "POST";
  request.body = submission.body;
  request.content_type = "application/x-www-form-urlencoded";
  request.cookie_jar = &cookies_;
  Result<net::HttpResponse> response = net::http_request(request, &diagnostics);
  if (response.is_error()) return Status::failure(response.error());
  if (response.value().status < 200 || response.value().status >= 300) {
    return io_failure("net.bad_status", "HTTP status " + std::to_string(response.value().status) + " for " + action);
  }
  std::string base = response.value().final_url.empty() ? action : response.value().final_url;
  std::string body = std::move(response.value().body);
  return load_html(std::move(body), base, base);
}

Status Session::submit_form(const bindings::FormSubmission& submission) {
  if (document_ == nullptr || !document_->is_alive(submission.form)) {
    return io_failure("shell.no_form", "the form is no longer in the document");
  }
  const std::string action = submission.action.empty() ? tab_.base : submission.action;
  const std::string resolved = action.empty() ? tab_.url : resolve_target(action);

  // `submit` listeners may cancel the submission (preventDefault()).
  if (bindings_ != nullptr && bindings_->dispatch(submission.form, "submit", js::Value::undefined())) {
    refresh_title();
    return Status::success();
  }

  if (submission.method == "POST") return load_posted(submission, resolved);
  std::string target = resolved;
  if (!submission.body.empty()) {
    target += (target.find('?') == std::string::npos ? "?" : "&");
    target += submission.body;
  }
  return load(target, "form");
}

bool Session::drain_pending_submissions() {
  if (bindings_ == nullptr) return false;
  bool submitted = false;
  for (const bindings::FormSubmission& submission : bindings_->take_pending_submissions()) {
    const Status status = submit_form(submission);
    submitted = true;
    if (status.is_error()) tab_.error = status.error().to_string();
    break;  // the first submission replaces the document; later ones are stale
  }
  return submitted;
}


// --------------------------------------------------------------- security -----

bool Session::allow_subresource(std::string_view kind, std::string_view url, Diagnostics* diagnostics) {
  const Result<net::Url> target = net::parse_url(std::string(url));
  if (target.is_error()) return true;  // not a URL we can judge; local handling decides

  const Result<net::Url> page = net::parse_url(tab_.url.empty() ? tab_.base : tab_.url);
  if (page.is_ok() && security::is_mixed_content(page.value(), target.value())) {
    if (diagnostics != nullptr) {
      diagnostics->warn(SourcePos{}, "security.mixed_content: blocked " + std::string(kind) + " " + std::string(url) +
                                         " (https page, http resource)");
    }
    network_log_.push_back(NetworkEntry{"GET", std::string(url), 0, 0, 0, 0.0, "security.mixed_content"});
    return false;
  }
  if (bindings_ != nullptr) {
    const security::Origin self = security::Origin::parse(tab_.origin);
    const security::CspPolicy& csp = bindings_->csp();
    const bool allowed = kind == "style"    ? csp.allows_style(url, self)
                         : kind == "image"  ? csp.allows_image(url, self)
                         : kind == "script" ? csp.allows_script(url, self)
                                            : true;
    if (!allowed) {
      if (diagnostics != nullptr) {
        diagnostics->warn(SourcePos{}, "security.csp: blocked " + std::string(kind) + " " + std::string(url));
      }
      network_log_.push_back(NetworkEntry{"GET", std::string(url), 0, 0, 0, 0.0, "security.csp"});
      return false;
    }
  }
  return true;
}

bindings::NetworkResponse Session::perform_xhr(const bindings::NetworkRequest& request) {
  bindings::NetworkResponse response;
  const Result<net::Url> url = net::parse_url(request.url);
  if (url.is_error()) {
    response.error = "net.bad_url";
    return response;
  }
  const security::Origin self = security::Origin::parse(tab_.origin);

  if (bindings_ != nullptr) {
    const security::CspPolicy& csp = bindings_->csp();
    if (!csp.allows_connect(request.url, self)) {
      response.error = "csp.blocked";
      network_log_.push_back(NetworkEntry{request.method, request.url, 0, request.body.size(), 0, 0.0, "csp.blocked"});
      return response;
    }
  }
  if (!allow_subresource("fetch", request.url, nullptr)) {
    response.error = "security.mixed_content";
    return response;
  }

  security::CorsRequest cors_request;
  cors_request.origin = self;
  cors_request.url = url.value();
  cors_request.method = request.method;
  for (const auto& [name, value] : request.headers) {
    (void)value;
    cors_request.header_names.push_back(name);
  }
  // Cross-origin XHR is not credentialed by default (like `fetch` without
  // `credentials: 'include'`), so `Access-Control-Allow-Origin: *` stays usable.
  cors_request.credentials = self == security::Origin::from_url(url.value());
  const security::CorsDecision decision = security::check_request(cors_request);
  if (decision.verdict == security::CorsVerdict::Blocked) {
    response.error = decision.reason;
    network_log_.push_back(NetworkEntry{request.method, request.url, 0, request.body.size(), 0, 0.0, decision.reason});
    return response;
  }
  if (decision.verdict == security::CorsVerdict::NeedsPreflight) {
    // We do not send OPTIONS yet; the request is refused instead of silently
    // downgrading to a non-CORS request.
    response.error = decision.reason;
    network_log_.push_back(NetworkEntry{request.method, request.url, 0, request.body.size(), 0, 0.0, decision.reason});
    return response;
  }

  const auto started = std::chrono::steady_clock::now();
  net::HttpRequest http_request;
  http_request.url = url.value();
  http_request.method = request.method;
  http_request.headers = request.headers;
  http_request.body = request.body;
  if (!request.body.empty()) http_request.content_type = "text/plain";
  http_request.cookie_jar = &cookies_;
  Diagnostics diagnostics;
  Result<net::HttpResponse> raw = net::http_request(http_request, &diagnostics);

  NetworkEntry entry;
  entry.method = request.method;
  entry.url = request.url;
  entry.request_bytes = request.body.size();

  if (raw.is_error()) {
    entry.blocked = raw.error().code;
    network_log_.push_back(std::move(entry));
    response.error = raw.error().code;
    return response;
  }

  const security::CorsResult cors =
      security::check_response(self, url.value(), cors_request.credentials, raw.value().headers);
  if (!cors.allowed) {
    entry.blocked = cors.reason;
    entry.status = raw.value().status;
    network_log_.push_back(std::move(entry));
    response.error = cors.reason;
    return response;
  }

  entry.status = raw.value().status;
  entry.response_bytes = raw.value().body.size();
  entry.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  network_log_.push_back(std::move(entry));

  response.ok = true;
  response.status = raw.value().status;
  response.status_text = raw.value().reason;
  response.headers = raw.value().headers;
  response.body = std::move(raw.value().body);
  return response;
}

DevToolsSnapshot Session::devtools() const {
  DevToolsSnapshot snapshot;
  snapshot.url = tab_.url;
  snapshot.title = tab_.title;
  snapshot.security = tab_.security;
  snapshot.origin = tab_.origin;
  snapshot.node_count = node_count_;
  snapshot.network = network_log_;
  snapshot.scripts_blocked = scripts_blocked_;
  if (bindings_ != nullptr) {
    snapshot.csp = bindings_->csp().describe();
    snapshot.scripts = bindings_->script_runs();
    for (const bindings::ScriptRun& run : bindings_->script_runs()) {
      if (!run.skipped) ++snapshot.scripts_run;
    }
  }
  if (js_context_ != nullptr) snapshot.console = js_context_->console();
  return snapshot;
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

  const layout::Box* hit = layout::hit_test(layout_->root, x, y + tab_.scroll_y, tab_.scroll_y);
  if (hit == nullptr) return outcome;

  dom::NodeId node = hit->node;
  if (bindings_ != nullptr) {
    const bool prevented = bindings_->dispatch(node, "click", js::Value::undefined());
    // A script may have called form.submit() from the handler.
    if (drain_pending_submissions()) {
      outcome.followed_link = true;
      outcome.href = tab_.url;
      outcome.needs_reload = tab_.state == LoadState::Ready;
      return outcome;
    }
    // Listeners may have changed the DOM: re-lay out so the next paint is honest.
    (void)rebuild_layout();
    set_scroll(tab_.scroll_y);
    if (prevented) return outcome;
    if (!document_->is_alive(node)) return outcome;
  }

  // Submit buttons: find the control under the cursor, then its form.
  for (dom::NodeId current = node; current.valid() && document_->is_alive(current);
       current = document_->parent(current)) {
    if (document_->type(current) != dom::NodeType::Element) continue;
    const std::string_view tag = document_->tag_name(current);
    const bool is_submit = (tag == "input" &&
                            (document_->attribute(current, "type") == "submit" ||
                             document_->attribute(current, "type") == "image")) ||
                           (tag == "button" && (document_->attribute(current, "type").empty() ||
                                                document_->attribute(current, "type") == "submit"));
    if (!is_submit) continue;
    dom::NodeId form;
    for (dom::NodeId parent = document_->parent(current); parent.valid(); parent = document_->parent(parent)) {
      if (document_->type(parent) == dom::NodeType::Element && document_->tag_name(parent) == "form") {
        form = parent;
        break;
      }
    }
    if (!form.valid() || bindings_ == nullptr) break;
    const Status status = submit_form(bindings_->collect_form_submission(form));
    outcome.needs_reload = status.is_ok();
    if (status.is_error()) tab_.error = status.error().to_string();
    return outcome;
  }

  // Walk up the box tree (each box keeps its DOM node) looking for a link.
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

const layout::Box* Session::box_of(dom::NodeId node) const {
  if (layout_ == nullptr || !node.valid()) return nullptr;
  std::vector<const layout::Box*> stack{&layout_->root};
  while (!stack.empty()) {
    const layout::Box* box = stack.back();
    stack.pop_back();
    if (box->node == node) return box;
    for (const layout::Box& child : box->children) stack.push_back(&child);
  }
  return nullptr;
}

const std::vector<bindings::ScriptRun>& Session::script_runs() const noexcept {
  static const std::vector<bindings::ScriptRun> empty;
  return bindings_ == nullptr ? empty : bindings_->script_runs();
}

const std::vector<js::ConsoleEntry>& Session::js_console() const noexcept {
  static const std::vector<js::ConsoleEntry> empty;
  return js_context_ == nullptr ? empty : js_context_->console();
}

bool Session::dispatch_event(dom::NodeId target, std::string_view type) {
  return bindings_ != nullptr && bindings_->dispatch(target, type);
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
