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
  std::string fragment_context;       // non-empty: the "#document-fragment" tag name
  bool scripting = false;             // "#script-on" cases need the scripting flag
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
  // Blank lines *inside* a `#document` section are the continuation lines of a text
  // node that holds a newline, so they carry meaning (`"\n\n"` and `"\n\n\n"`
  // print differently). Only the blank lines that separate the section from the next
  // `#...` header are dropped — hence the trim on every section change.
  const auto trim_trailing = [&current] {
    while (!current.expected.empty() && current.expected.back().empty()) current.expected.pop_back();
  };
  const auto flush = [&cases, &current, &trim_trailing] {
    trim_trailing();
    if (!current.expected.empty() || !current.data.empty()) cases.push_back(current);
    current = Case{};
  };
  const auto is_header = [](const std::string& line) {
    return line == "#data" || line == "#errors" || line == "#new-errors" || line == "#document" ||
           line == "#document-fragment" || line == "#script-on" || line == "#script-off";
  };
  for (const std::string& line : lines) {
    if (in_document && !is_header(line)) {
      // Indentation matters: html5lib prints the continuation lines of a text node
      // that contains a newline verbatim, leading spaces and all. Only a CR from a
      // CRLF checkout is stripped.
      std::string_view kept = line;
      while (!kept.empty() && kept.back() == '\r') kept.remove_suffix(1);
      current.expected.emplace_back(kept);
      continue;
    }
    if (line == "#data") {
      if (!current.expected.empty()) {  // previous case without a #document section
        flush();
      }
      in_data = true;
      in_document = false;
      in_errors = false;
      continue;
    }
    if (line == "#document") {
      trim_trailing();
      in_data = false;
      in_document = true;
      in_errors = false;
      continue;
    }
    if (line == "#errors" || line == "#new-errors") {
      trim_trailing();
      in_data = false;
      in_document = false;
      in_errors = true;
      continue;
    }
    if (line == "#document-fragment") {
      trim_trailing();
      current.fragment_context = "?";  // marker: the next line holds the tag name
      in_data = false;
      in_document = false;
      in_errors = false;
      continue;
    }
    if (line == "#script-on" || line == "#script-off") {
      if (!current.expected.empty()) {
        flush();
      }
      current.scripting = line == "#script-on";
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
    if (current.fragment_context == "?" && !line.empty()) {
      current.fragment_context = trim_copy(line);  // the context element's tag name
      continue;
    }
    (void)in_errors;
  }
  flush();
  return cases;
}

/// html5lib's expected files print attribute values verbatim (no escaping of `&`
/// or `"`), so the comparison must do the same.

/// Pushes `text` as lines: the first one indented, continuation lines verbatim (the
/// .dat files are generated from a single string, so embedded newlines are not
/// re-indented).
void push_split(std::vector<std::string>& out, const std::string& indent, std::string_view text) {
  std::size_t start = 0;
  for (;;) {
    const std::size_t newline = text.find('\n', start);
    const std::string_view piece =
        newline == std::string_view::npos ? text.substr(start) : text.substr(start, newline - start);
    out.push_back((start == 0 ? indent : std::string{}) + std::string(piece));
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
}

void serialize(const dom::Document& document, dom::NodeId node, std::size_t depth,
               std::vector<std::string>& out) {
  const std::string indent = "| " + std::string(depth * 2, ' ');
  switch (document.type(node)) {
    case dom::NodeType::Document:
      for (const dom::NodeId child : document.children(node)) serialize(document, child, 0, out);
      return;
    case dom::NodeType::Doctype: {
      // html5lib prints the identifiers verbatim once the doctype has any:
      // `<!DOCTYPE html "-//W3C//DTD HTML 4.01//EN" "http://…/strict.dtd">`.
      const std::string_view name = document.text_data(node);
      std::string line = indent + "<!DOCTYPE " + std::string(name);
      if (document.has_doctype_identifiers(node)) {
        line += " \"" + std::string(document.doctype_public_id(node)) + "\" \"" +
                std::string(document.doctype_system_id(node)) + "\"";
      }
      line += ">";
      out.push_back(line);
      return;
    }
    case dom::NodeType::Comment:
      // html5lib prints the comment (newlines and all) as one string; the expected
      // files are line based, so split it the same way they were written.
      push_split(out, indent, "<!-- " + std::string(document.text_data(node)) + " -->");
      return;
    case dom::NodeType::Text:
      // The corpus prints a text node as one quoted string; a newline inside it is
      // written verbatim, so continuation lines keep no indentation.
      push_split(out, indent, "\"" + std::string(document.text_data(node)) + "\"");
      return;
    case dom::NodeType::Fragment: {  // <template> contents
      out.push_back(indent + "content");
      for (const dom::NodeId child : document.children(node)) serialize(document, child, depth + 1, out);
      return;
    }
    case dom::NodeType::Element: {
      std::vector<std::string> attributes;
      for (const dom::Attribute& attribute : document.attributes(node)) {
        // Namespaced attributes print as `prefix localName`, everything else as
        // `name` — exactly the html5lib outline form.
        const std::string name = attribute.prefix.empty()
                                     ? std::string(attribute.name)
                                     : std::string(attribute.prefix) + " " + std::string(attribute.name);
        attributes.push_back(name + "=\"" + std::string(attribute.value) + "\"");
      }
      std::sort(attributes.begin(), attributes.end());
      // Foreign elements are namespace-qualified in the outline: `<svg svg>`,
      // `<math mi>`.
      // html5lib writes the namespace first: `<svg font>`, `<math mi>`.
      std::string tag;
      if (document.element_namespace(node) != dom::Namespace::Html) {
        tag += dom::namespace_name(document.element_namespace(node));
        tag += " ";
      }
      tag += document.tag_name(node);
      out.push_back(indent + "<" + tag + ">");
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

  for (const std::string& path : detail::list_files(directory, ".dat")) {
    const std::string text = detail::read_file(path);
    if (text.empty()) continue;
    const std::string file = std::filesystem::path(path).filename().string();
    std::size_t index = 0;
    for (const Case& test : parse_dat(text)) {
      ++index;
      if (options.limit != 0 && report.total >= options.limit) {
        report.notes.push_back("fragment cases: " + std::to_string(fragment_cases));
        return report;
      }
      if (test.expected.empty() && test.data.empty()) continue;
      ++report.total;

      dom::Document document;
      html::ParseOptions parse_options;
      parse_options.scripting_enabled = test.scripting;
      std::vector<std::string> actual;
      const bool fragment = !test.fragment_context.empty() && test.fragment_context != "?";
      if (fragment) {
        // Fragment parsing: the expected tree is the *children* of the context
        // element (html5lib prints the fragment's nodes, not a wrapper).
        ++fragment_cases;
        const dom::NodeId context =
            html::parse_fragment(test.data, test.fragment_context, document, nullptr, parse_options);
        for (const dom::NodeId child : document.children(context)) serialize(document, child, 0, actual);
      } else {
        (void)html::parse_html(test.data, document, nullptr, parse_options);
        serialize(document, document.root(), 0, actual);
      }

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
        const std::string label = file + " #" + std::to_string(index) +
                                  (fragment ? " (fragment " + test.fragment_context + ")" : "");
        report.failures.push_back({label, detail_text + " [input " + test.data + "]"});
      }
    }
  }
  report.notes.push_back("fragment cases: " + std::to_string(fragment_cases));
  return report;
}

}  // namespace bonk::conformance
