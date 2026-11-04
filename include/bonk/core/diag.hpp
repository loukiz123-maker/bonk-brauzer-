// Bonk Brauzer — diagnostic sink shared by every parser.
//
// HTML/CSS/JS parsing is *error tolerant*: a malformed page must still render.
// So parsers do not fail fast; they call `error()`/`warn()` and keep going.
// The sink is a plain vector of records that the shell (console panel) and the
// devtools can read later, which keeps parsers free of UI dependencies.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bonk {

enum class Severity { Hint, Warning, Error };

[[nodiscard]] std::string_view severity_name(Severity severity) noexcept;

/// A position inside a source document. `offset` is a byte offset (what the
/// tokenizers advance), `line`/`column` are 1-based and only filled in when a
/// consumer needs them (they cost a scan to compute).
struct SourcePos {
  std::string_view source;  // must outlive the diagnostic record (interned url/name)
  std::size_t offset = 0;
  std::size_t line = 0;
  std::size_t column = 0;

  [[nodiscard]] static SourcePos at(std::size_t offset) noexcept { return SourcePos{{}, offset, 0, 0}; }
  [[nodiscard]] static SourcePos at_line(std::string_view source, std::size_t line, std::size_t column) noexcept {
    return SourcePos{source, 0, line, column};
  }
};

struct Diagnostic {
  Severity severity = Severity::Error;
  SourcePos position;
  std::string message;
};

class Diagnostics {
 public:
  void add(Severity severity, SourcePos position, std::string message);
  void error(SourcePos position, std::string message) { add(Severity::Error, position, std::move(message)); }
  void warn(SourcePos position, std::string message) { add(Severity::Warning, position, std::move(message)); }

  [[nodiscard]] const std::vector<Diagnostic>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] std::size_t count(Severity severity) const noexcept;
  [[nodiscard]] bool has_errors() const noexcept { return count(Severity::Error) > 0; }
  void clear() noexcept { items_.clear(); }

 private:
  std::vector<Diagnostic> items_;
};

/// "input.js:3:5: error: unexpected token" (fields omitted when unknown).
[[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);

/// One line per diagnostic plus a summary line when there is more than one.
[[nodiscard]] std::string format_diagnostics(const Diagnostics& diagnostics);

}  // namespace bonk
