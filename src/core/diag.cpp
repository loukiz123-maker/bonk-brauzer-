#include "bonk/core/diag.hpp"

namespace bonk {

std::string_view severity_name(Severity severity) noexcept {
  switch (severity) {
    case Severity::Hint:
      return "hint";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
  }
  return "error";
}

void Diagnostics::add(Severity severity, SourcePos position, std::string message) {
  items_.push_back(Diagnostic{severity, position, std::move(message)});
}

std::size_t Diagnostics::count(Severity severity) const noexcept {
  std::size_t total = 0;
  for (const Diagnostic& diagnostic : items_) {
    if (diagnostic.severity == severity) ++total;
  }
  return total;
}

std::string format_diagnostic(const Diagnostic& diagnostic) {
  std::string out;
  const SourcePos& position = diagnostic.position;
  if (!position.source.empty() || position.line > 0) {
    out += position.source.empty() ? "<input>" : std::string(position.source);
    if (position.line > 0) {
      out += ':';
      out += std::to_string(position.line);
      out += ':';
      out += std::to_string(position.column > 0 ? position.column : 1);
    }
    out += ": ";
  }
  out += severity_name(diagnostic.severity);
  out += ": ";
  out += diagnostic.message;
  return out;
}

std::string format_diagnostics(const Diagnostics& diagnostics) {
  std::string out;
  for (const Diagnostic& diagnostic : diagnostics.items()) {
    out += format_diagnostic(diagnostic);
    out += '\n';
  }
  if (diagnostics.size() > 1) {
    out += std::to_string(diagnostics.count(Severity::Error));
    out += " error(s), ";
    out += std::to_string(diagnostics.count(Severity::Warning));
    out += " warning(s), ";
    out += std::to_string(diagnostics.count(Severity::Hint));
    out += " hint(s)\n";
  }
  return out;
}

}  // namespace bonk
