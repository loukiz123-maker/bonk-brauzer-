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
#include "bonk/js/js.hpp"

namespace bonk::bindings {

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
