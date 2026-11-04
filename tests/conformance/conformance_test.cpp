// Conformance harness: the JSON reader, the three corpus runners (against the
// small in-repo fixtures so the unit suite stays offline) and the report writers.
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "bonk/conformance/conformance.hpp"

using namespace bonk::conformance;

namespace {

bool file_exists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  std::fclose(file);
  return true;
}

}  // namespace

BONK_TEST(conformance, json_reader_handles_the_corpus_shape) {
  // The reader itself is internal to the conformance library; the fixture file
  // covers what the corpus needs: nested arrays, attribute objects, \u escapes
  // and the `doubleEscaped` flag (which unescapes one more time).
  CHECK(detail::directory_exists("tests/conformance/data"));
  const std::string text = detail::read_file("tests/conformance/data/tokenizer/sample.test");
  CHECK(!text.empty());
  CHECK(text.find("doubleEscaped") != std::string::npos);

  const SuiteReport report = run_tokenizer_corpus("tests/conformance/data/tokenizer");
  CHECK_EQ(report.failed(), static_cast<std::size_t>(0));
  CHECK(report.passed >= 8);  // every fixture case, including the doubleEscaped one
}

BONK_TEST(conformance, tokenizer_corpus_runs_the_in_repo_fixture) {
  SuiteReport report = run_tokenizer_corpus("tests/conformance/data/tokenizer");
  CHECK_EQ(report.total, static_cast<std::size_t>(8));
  CHECK_EQ(report.failed(), static_cast<std::size_t>(0));
  CHECK_EQ(report.passed, report.total - report.skipped);
}

BONK_TEST(conformance, tokenizer_corpus_missing_directory_is_not_an_error) {
  const SuiteReport report = run_tokenizer_corpus("tests/conformance/data/does-not-exist");
  CHECK_EQ(report.total, static_cast<std::size_t>(0));
  CHECK_EQ(report.passed, static_cast<std::size_t>(0));
}

BONK_TEST(conformance, tree_corpus_matches_the_expected_outlines) {
  const SuiteReport report = run_tree_corpus("tests/conformance/data/tree");
  CHECK_EQ(report.total, static_cast<std::size_t>(3));
  CHECK_EQ(report.passed, static_cast<std::size_t>(3));
  CHECK_EQ(report.failed(), static_cast<std::size_t>(0));
}

BONK_TEST(conformance, test262_corpus_runs_positive_and_negative_cases) {
  const SuiteReport report = run_test262_corpus("tests/conformance/data/test262");
  CHECK_EQ(report.total, static_cast<std::size_t>(2));
  CHECK_EQ(report.passed, static_cast<std::size_t>(2));
}

BONK_TEST(conformance, reftests_compare_pixels) {
  const SuiteReport report = run_reftest_corpus("tests/conformance/reftests");
  CHECK(report.total >= 5);
  CHECK_EQ(report.failed(), static_cast<std::size_t>(0));
  CHECK_EQ(report.passed, report.total);
}

BONK_TEST(conformance, reports_render_markdown_and_json) {
  SuiteReport report;
  report.suite = "demo";
  report.source = "somewhere";
  report.total = 4;
  report.passed = 3;
  report.skipped = 1;
  report.failures.push_back({"case-1", "expected 'a' | got 'b'"});

  const std::string markdown = to_markdown({report});
  CHECK(markdown.find("| demo | 4 | 3 | 0 | 1 | 0 | 75.0% |") != std::string::npos);
  CHECK(markdown.find("case-1") != std::string::npos);

  const std::string json = to_json({report});
  CHECK(json.find("\"suite\": \"demo\"") != std::string::npos);
  CHECK(json.find("\"pass_rate\"") != std::string::npos);
  CHECK(json.find("\"case\": \"case-1\"") != std::string::npos);
}

BONK_TEST(conformance, fixtures_are_present) {
  CHECK(file_exists("tests/conformance/data/tokenizer/sample.test"));
  CHECK(file_exists("tests/conformance/data/tree/sample.dat"));
  CHECK(file_exists("tests/conformance/data/test262/language/statements/pass.js"));
  CHECK(file_exists("tests/conformance/reftests/flex-row.html"));
  CHECK(file_exists("tests/conformance/reftests/flex-row-ref.html"));
}
