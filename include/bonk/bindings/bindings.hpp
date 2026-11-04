// Bonk Brauzer — JS <-> DOM bridge (stage 4).
//
// Design: the bridge owns *all* JavaScript-visible state in side tables keyed by
// dom::NodeId (never inside the DOM nodes), so the DOM stays POD and testable:
//   * wrappers      NodeId -> the single JS object for that node (identity is
//                   preserved: `el === el` is true, so scripts can compare nodes);
//   * listeners     NodeId -> {type -> handlers}, dispatched with real bubbling;
//   * scripts       <script> elements executed in document order on load.
// Accessors (getters/setters on js::Object) make `el.textContent = 'x'` write
// through to the document without adding a special object kind to the engine.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"
#include "bonk/dom/dom.hpp"
#include "bonk/image/image.hpp"
#include "bonk/js/js.hpp"
#include "bonk/net/cookie.hpp"
#include "bonk/security/security.hpp"
#include "bonk/text/font.hpp"

namespace bonk::bindings {

/// Key/value store behind `localStorage` / `sessionStorage`.
///
/// Values are strings (the Web Storage API contract), keys keep insertion order,
/// and an optional file makes `localStorage` survive restarts. The file format is
/// one escaped `key<TAB>value` pair per line — no JSON dependency, easy to diff.
class Storage {
 public:
  explicit Storage(std::string path = {});

  [[nodiscard]] std::string get(const std::string& key) const;      // "" when absent
  [[nodiscard]] bool has(const std::string& key) const;
  void set(std::string key, std::string value);
  void remove(const std::string& key);
  void clear();
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::string key_of(std::size_t index) const;
  [[nodiscard]] std::vector<std::string> keys() const;

  void set_path(std::string path) { path_ = std::move(path); }
  [[nodiscard]] bool load();
  [[nodiscard]] bool save() const;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
  std::vector<std::pair<std::string, std::string>> entries_;
};

/// A `<canvas>` bitmap after the page's drawing calls, ready for layout/paint.
struct CanvasImage {
  dom::NodeId node;
  image::Image image;
};

/// A form ready to be submitted: method, resolved-by-the-shell action and an
/// `application/x-www-form-urlencoded` body.
struct FormSubmission {
  dom::NodeId form;
  std::string method = "GET";
  std::string action;
  std::string body;
};

/// One script-driven network request (the shell performs it, bindings never talk
/// to sockets: it keeps the JS layer testable and the policy in one place).
struct NetworkRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct NetworkResponse {
  bool ok = false;
  int status = 0;
  std::string status_text;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string error;  // "cors.blocked", "csp.blocked", "net.timeout", …
};

using NetworkHandler = std::function<NetworkResponse(const NetworkRequest&)>;

/// Outcome of one <script> element.
struct ScriptRun {
  std::string url;    // document URL + script index
  bool ok = true;
  bool skipped = false;  // e.g. external `src` scripts, which we cannot fetch yet
  std::string message;
};

class Bindings {
 public:
  Bindings(js::Context& context, dom::Document& document);
  ~Bindings();
  Bindings(const Bindings&) = delete;
  Bindings& operator=(const Bindings&) = delete;

  /// Creates `window`, `document` and friends in the JS context.
  void install();

  /// Executes every inline <script> in document order (external scripts are skipped).
  void run_scripts();

  /// Fires `type` at `target`, bubbling up to the document root.
  /// Returns true when a listener called preventDefault().
  bool dispatch(dom::NodeId target, std::string_view type, js::Value detail = js::Value::undefined());

  /// The single JS wrapper object for a node (creates it on first use).
  [[nodiscard]] js::Value wrap(dom::NodeId node);
  /// Reverse mapping: the NodeId behind a wrapper, when `value` is one.
  [[nodiscard]] bool node_of(const js::Value& value, dom::NodeId& out) const;

  /// Font used by `fillText` (a default fallback font is used when unset).
  void set_font(const text::Font* font);

  /// Cookie jar backing `document.cookie` (null = cookies disabled).
  void set_cookie_jar(net::CookieJar* jar);

  /// Content-Security-Policy for this document (empty policy = no restrictions).
  void set_csp(security::CspPolicy policy);
  [[nodiscard]] const security::CspPolicy& csp() const noexcept;

  /// Installs the transport used by `XMLHttpRequest` (the shell owns the network).
  void set_network_handler(NetworkHandler handler);

  /// Storage objects exposed as `localStorage` / `sessionStorage`.
  [[nodiscard]] Storage& local_storage() noexcept;
  [[nodiscard]] Storage& session_storage() noexcept;

  /// Reads a form's successful controls into a submission (no I/O).
  [[nodiscard]] FormSubmission collect_form_submission(dom::NodeId form) const;

  /// Submissions queued by `form.submit()` inside scripts (the shell drains them).
  [[nodiscard]] std::vector<FormSubmission> take_pending_submissions();

  /// Copies control values into text nodes so the layout can render them
  /// (`<input value="x">`, `<textarea>…</textarea>`, the selected `<option>`).
  void materialize_form_controls();

  /// Rebuilds the canvas bitmaps after scripts/handlers drew something.
  void refresh_canvases();
  [[nodiscard]] const std::vector<CanvasImage>& canvas_images() const noexcept;

  [[nodiscard]] js::Context& context() noexcept { return *context_; }
  [[nodiscard]] dom::Document& document() noexcept { return *document_; }
  [[nodiscard]] const std::vector<ScriptRun>& script_runs() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  js::Context* context_ = nullptr;
  dom::Document* document_ = nullptr;
};

}  // namespace bonk::bindings
