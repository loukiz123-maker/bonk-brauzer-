// html5lib tree-construction corpus runner.
//
// Corpus: html5lib-tests `tree-construction/*.dat` (the pre-2019 revision of the
// repository is what still ships these files; they now live in web-platform-tests
// under html/syntax/parsing). Each case looks like:
//
//   #data
//   <p>One<p>Two
//   #errors
//   (1,3): expected-doctype-but-got-start-tag
//   #document
//   | <html>
//   |   <head>
//   |   <body>
//   |     <p>
//   |       "One"
//
// We compare the expected indented outline with our own serialisation of the same
// shape. Error lines are ignored (we do not model the parse-error stream), and
// cases that need fragment parsing or doctype identifiers are counted as skipped.
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/conformance/conformance.hpp"
#include "bonk/dom/dom.hpp"
#include "bonk/html/html.hpp"

namespace bonk::conformance {
namespace {

struct Case {
  std::string name;
  std::string data;
  std::vector<std::string> expected;  // lines of the "#document" section
  bool fragment = false;
};

[[nodiscard]] std::string trim_copy(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
  return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] std::vector<Case> parse_dat(std::string_view text) {
  std::vector<Case> cases;
  std::vector<std::string> lines;
  {
    std::size_t start = 0;
    while (start <= text.size()) {
      const std::size_t newline = text.find('\n', start);
      const std::size_t length = newline == std::string_view::npos ? text.size() - start : newline - start;
      lines.emplace_back(text.substr(start, length));
      if (newline == std::string_view::npos) break;
      start = newline + 1;
    }
  }

  Case current;
  bool in_data = false;
  bool in_document = false;
  bool in_errors = false;
  for (const std::string& line : lines) {
    if (line == "#data") {
      if (!current.expected.empty()) {  // previous case without a #document section
        cases.push_back(current);
        current = Case{};
      }
      in_data = true;
      in_document = false;
      in_errors = false;
      continue;
    }
    if (line == "#document") {
      in_data = false;
      in_document = true;
      in_errors = false;
      continue;
    }
    if (line == "#errors" || line == "#new-errors") {
      in_data = false;
      in_document = false;
      in_errors = true;
      continue;
    }
    if (line == "#document-fragment") {
      current.fragment = true;
      in_data = false;
      in_document = false;
      in_errors = false;
      continue;
    }
    if (line == "#script-on" || line == "#script-off") {
      if (!current.expected.empty()) {
        cases.push_back(current);
        current = Case{};
      }
      in_data = false;
      in_document = false;
      in_errors = false;
      continue;
    }
    if (in_data) {
      if (!current.data.empty()) current.data += '\n';
      current.data += line;
      continue;
    }
    if (in_document) {
      if (line.empty()) continue;
      current.expected.push_back(trim_copy(line));
      continue;
    }
    (void)in_errors;
  }
  if (!current.expected.empty() || !current.data.empty()) cases.push_back(current);
  return cases;
}

/// html5lib's expected files print attribute values verbatim (no escaping of `&`
/// or `"`), so the comparison must do the same.

void serialize(const dom::Document& document, dom::NodeId node, std::size_t depth,
               std::vector<std::string>& out) {
  const std::string indent = "| " + std::string(depth * 2, ' ');
  switch (document.type(node)) {
    case dom::NodeType::Document:
      for (const dom::NodeId child : document.children(node)) serialize(document, child, 0, out);
      return;
    case dom::NodeType::Doctype: {
      const std::string_view name = document.text_data(node);
      out.push_back(indent + "<!DOCTYPE " + std::string(name) + ">");
      return;
    }
    case dom::NodeType::Comment:
      out.push_back(indent + "<!-- " + std::string(document.text_data(node)) + " -->");
      return;
    case dom::NodeType::Text: {
      const std::string_view text = document.text_data(node);
      std::size_t start = 0;
      for (;;) {
        const std::size_t newline = text.find('\n', start);
        const std::string_view piece =
            newline == std::string_view::npos ? text.substr(start) : text.substr(start, newline - start);
        out.push_back(indent + "\"" + std::string(piece) + "\"");
        if (newline == std::string_view::npos) break;
        start = newline + 1;
      }
      return;
    }
    case dom::NodeType::Element: {
      std::vector<std::string> attributes;
      for (const dom::Attribute& attribute : document.attributes(node)) {
        attributes.push_back(std::string(attribute.name) + "=\"" + std::string(attribute.value) + "\"");
      }
      std::sort(attributes.begin(), attributes.end());
      out.push_back(indent + "<" + std::string(document.tag_name(node)) + ">");
      const std::string attribute_indent = "| " + std::string((depth + 1) * 2, ' ');
      for (const std::string& attribute : attributes) out.push_back(attribute_indent + attribute);
      for (const dom::NodeId child : document.children(node)) serialize(document, child, depth + 1, out);
      return;
    }
  }
}

}  // namespace

SuiteReport run_tree_corpus(std::string_view directory, const SuiteOptions& options) {
  SuiteReport report;
  report.suite = "html5lib-tree-construction";
  report.source = std::string(directory);
  if (!detail::directory_exists(directory)) return report;

  std::size_t fragment_cases = 0;
  std::size_t doctype_id_cases = 0;
  std::size_t foreign_cases = 0;

  for (const std::string& path : detail::list_files(directory, ".dat")) {
    const std::string text = detail::read_file(path);
    if (text.empty()) continue;
    const std::string file = std::filesystem::path(path).filename().string();
    std::size_t index = 0;
    for (const Case& test : parse_dat(text)) {
      ++index;
      if (options.limit != 0 && report.total >= options.limit) {
        report.notes.push_back("skipped fragment cases: " + std::to_string(fragment_cases));
        report.notes.push_back("skipped doctype-identifier cases: " + std::to_string(doctype_id_cases));
        report.notes.push_back("skipped foreign-content cases: " + std::to_string(foreign_cases));
        return report;
      }
      if (test.expected.empty() && test.data.empty()) continue;
      if (test.fragment) {
        ++fragment_cases;
        continue;
      }
      bool needs_doctype_ids = false;
      bool needs_foreign = false;
      for (const std::string& line : test.expected) {
        if (line.find("<!DOCTYPE") != std::string::npos && line.find('"') != std::string::npos) {
          needs_doctype_ids = true;
        }
        // html5lib prints foreign elements namespace-qualified: "<svg svg>", "<math math>".
        if (line.find("<svg ") != std::string::npos || line.find("<math ") != std::string::npos) {
          needs_foreign = true;
        }
      }
      if (needs_doctype_ids) {
        ++doctype_id_cases;
        continue;
      }
      if (needs_foreign) {
        ++foreign_cases;
        continue;
      }
      ++report.total;

      dom::Document document;
      html::ParseOptions parse_options;
      (void)html::parse_html(test.data, document, nullptr, parse_options);
      std::vector<std::string> actual;
      serialize(document, document.root(), 0, actual);

      if (actual == test.expected) {
        ++report.passed;
        continue;
      }
      if (report.failures.size() < options.max_failures) {
        std::string detail_text;
        std::size_t line = 0;
        while (line < actual.size() && line < test.expected.size() && actual[line] == test.expected[line]) ++line;
        if (line < actual.size() && line < test.expected.size()) {
          detail_text = "line " + std::to_string(line + 1) + ": expected '" + test.expected[line] + "', got '" +
                        actual[line] + "'";
        } else if (actual.size() > test.expected.size()) {
          detail_text = "extra node: '" + actual[test.expected.size()] + "'";
        } else {
          detail_text = "missing node: expected '" + test.expected[actual.size()] + "'";
        }
        report.failures.push_back({file + " #" + std::to_string(index), detail_text + " [input " + test.data + "]"});
      }
    }
  }
  report.notes.push_back("skipped fragment cases: " + std::to_string(fragment_cases));
  report.notes.push_back("skipped doctype-identifier cases: " + std::to_string(doctype_id_cases));
  report.notes.push_back("skipped foreign-content cases: " + std::to_string(foreign_cases));
  report.excluded = fragment_cases + doctype_id_cases + foreign_cases;
  return report;
}

}  // namespace bonk::conformance
