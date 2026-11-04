#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "bonk/conformance/conformance.hpp"

namespace bonk::conformance {
namespace {

[[nodiscard]] std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

[[nodiscard]] std::string percent(double value) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%.1f%%", value);
  return buffer;
}

[[nodiscard]] std::string markdown_cell(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '|') {
      out += "\\|";
    } else if (c == '\n' || c == '\r') {
      out.push_back(' ');
    } else if (c == '`') {
      out.push_back('\'');
    } else {
      out.push_back(c);
    }
  }
  if (out.size() > 160) {
    out.resize(157);
    out += "...";
  }
  return out;
}

}  // namespace

std::string to_markdown(const std::vector<SuiteReport>& reports) {
  std::string out;
  out += "| suite | cases | passed | failed | skipped | excluded | pass rate |\n";
  out += "|---|---:|---:|---:|---:|---:|---:|\n";
  for (const SuiteReport& report : reports) {
    out += "| " + report.suite + " | " + std::to_string(report.total) + " | " + std::to_string(report.passed) + " | " +
           std::to_string(report.failed()) + " | " + std::to_string(report.skipped) + " | " +
           std::to_string(report.excluded) + " | " + percent(report.pass_rate()) + " |\n";
  }
  for (const SuiteReport& report : reports) {
    out += "\n### " + report.suite + "\n\n";
    if (report.total == 0) {
      out += "Corpus not present (`" + report.source + "`).\n";
      continue;
    }
    out += "Corpus: `" + report.source + "` — " + std::to_string(report.total) + " cases, " +
           std::to_string(report.passed) + " passed, " + std::to_string(report.failed()) + " failed, " +
           std::to_string(report.skipped) + " skipped, " + std::to_string(report.excluded) +
           " outside the runnable set (" + percent(report.pass_rate()) + " pass rate over the runnable cases).\n";
    if (!report.notes.empty()) {
      out += "\n";
      for (const std::string& note : report.notes) out += "* " + note + "\n";
    }
    if (!report.failures.empty()) {
      out += "\n| case | detail |\n|---|---|\n";
      for (const CaseFailure& failure : report.failures) {
        out += "| `" + markdown_cell(failure.name) + "` | " + markdown_cell(failure.detail) + " |\n";
      }
    }
  }
  return out;
}

std::string to_json(const std::vector<SuiteReport>& reports) {
  std::string out = "{\n  \"suites\": [\n";
  for (std::size_t i = 0; i < reports.size(); ++i) {
    const SuiteReport& report = reports[i];
    out += "    {\n";
    out += "      \"suite\": \"" + json_escape(report.suite) + "\",\n";
    out += "      \"source\": \"" + json_escape(report.source) + "\",\n";
    out += "      \"total\": " + std::to_string(report.total) + ",\n";
    out += "      \"passed\": " + std::to_string(report.passed) + ",\n";
    out += "      \"failed\": " + std::to_string(report.failed()) + ",\n";
    out += "      \"skipped\": " + std::to_string(report.skipped) + ",\n";
    out += "      \"excluded\": " + std::to_string(report.excluded) + ",\n";
    out += "      \"pass_rate\": " + std::to_string(report.pass_rate()) + ",\n";
    out += "      \"notes\": [";
    for (std::size_t n = 0; n < report.notes.size(); ++n) {
      out += (n == 0 ? "" : ", ") + ("\"" + json_escape(report.notes[n]) + "\"");
    }
    out += "],\n      \"failures\": [";
    for (std::size_t f = 0; f < report.failures.size(); ++f) {
      out += f == 0 ? "\n" : ",\n";
      out += "        {\"case\": \"" + json_escape(report.failures[f].name) + "\", \"detail\": \"" +
             json_escape(report.failures[f].detail) + "\"}";
    }
    out += report.failures.empty() ? "]\n" : "\n      ]\n";
    out += i + 1 == reports.size() ? "    }\n" : "    },\n";
  }
  out += "  ]\n}\n";
  return out;
}

}  // namespace bonk::conformance
