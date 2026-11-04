#include "test_framework.hpp"

#include "bonk/core/diag.hpp"

using namespace bonk;

BONK_TEST(diag, counts_and_flags) {
  Diagnostics diagnostics;
  CHECK(!diagnostics.has_errors());
  diagnostics.error(SourcePos::at(0), "bad");
  diagnostics.warn(SourcePos::at(1), "meh");
  diagnostics.warn(SourcePos::at(2), "meh");
  diagnostics.add(Severity::Hint, SourcePos::at(3), "fyi");

  CHECK_EQ(diagnostics.size(), std::size_t{4});
  CHECK_EQ(diagnostics.count(Severity::Error), std::size_t{1});
  CHECK_EQ(diagnostics.count(Severity::Warning), std::size_t{2});
  CHECK_EQ(diagnostics.count(Severity::Hint), std::size_t{1});
  CHECK(diagnostics.has_errors());
}

BONK_TEST(diag, format_with_full_position) {
  Diagnostics diagnostics;
  diagnostics.error(SourcePos::at_line("app.js", 3, 9), "unexpected token '}'");
  REQUIRE(diagnostics.size() == 1);
  CHECK_EQ(format_diagnostic(diagnostics.items()[0]), std::string("app.js:3:9: error: unexpected token '}'"));
}

BONK_TEST(diag, format_without_position) {
  Diagnostics diagnostics;
  diagnostics.warn(SourcePos{}, "unknown CSS property 'colour'");
  CHECK_EQ(format_diagnostic(diagnostics.items()[0]),
           std::string("warning: unknown CSS property 'colour'"));
}

BONK_TEST(diag, format_with_line_only) {
  Diagnostics diagnostics;
  diagnostics.add(Severity::Hint, SourcePos::at_line("<input>", 7, 0), "recovered");
  CHECK_EQ(format_diagnostic(diagnostics.items()[0]), std::string("<input>:7:1: hint: recovered"));
}

BONK_TEST(diag, format_all_adds_summary) {
  Diagnostics diagnostics;
  diagnostics.error(SourcePos::at(0), "a");
  diagnostics.warn(SourcePos::at(0), "b");
  const std::string text = format_diagnostics(diagnostics);
  CHECK_EQ(text, std::string("error: a\nwarning: b\n1 error(s), 1 warning(s), 0 hint(s)\n"));
}

BONK_TEST(diag, single_diagnostic_has_no_summary) {
  Diagnostics diagnostics;
  diagnostics.error(SourcePos::at(0), "only");
  CHECK_EQ(format_diagnostics(diagnostics), std::string("error: only\n"));
}

BONK_TEST(diag, clear) {
  Diagnostics diagnostics;
  diagnostics.error(SourcePos::at(0), "x");
  diagnostics.clear();
  CHECK_EQ(diagnostics.size(), std::size_t{0});
  CHECK(!diagnostics.has_errors());
}

BONK_TEST(diag, severity_names) {
  CHECK_EQ(severity_name(Severity::Hint), std::string_view("hint"));
  CHECK_EQ(severity_name(Severity::Warning), std::string_view("warning"));
  CHECK_EQ(severity_name(Severity::Error), std::string_view("error"));
}
