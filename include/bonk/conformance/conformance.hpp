// Bonk Brauzer — conformance harness (stage 7).
//
// Three corpora, one report shape:
//   * html5lib tokenizer tests   (JSON, thousands of tiny cases)
//   * html5lib tree construction (.dat, expected DOM as an indented tree)
//   * test262                    (JS sources + `/*--- … ---*/` metadata)
// plus our own pixel reftests (`<name>.html` against `<name>-ref.html`).
//
// The harness never downloads anything: `tools/fetch_conformance.sh` does that
// into a directory outside the repository, and every runner simply skips when
// the corpus is absent — so `make test` stays offline and reproducible.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bonk::conformance {

struct CaseFailure {
  std::string name;
  std::string detail;  // "expected … got …" or an error message, already trimmed
};

struct SuiteReport {
  std::string suite;   // "html5lib-tokenizer", "test262", …
  std::string source;  // corpus directory
  std::size_t total = 0;
  std::size_t passed = 0;
  std::size_t skipped = 0;   // cases inside `total` that we deliberately do not model
  std::size_t excluded = 0;  // cases outside the runnable set (foreign content, features we lack, …)
  std::vector<CaseFailure> failures;  // capped by SuiteOptions::max_failures
  std::vector<std::string> notes;     // per-runner commentary ("skipped: …")

  [[nodiscard]] double pass_rate() const noexcept {
    return total == 0 ? 0.0 : 100.0 * static_cast<double>(passed) / static_cast<double>(total);
  }
  [[nodiscard]] std::size_t failed() const noexcept { return total - passed - skipped; }
};

struct SuiteOptions {
  std::size_t limit = 0;          // 0 = every case in the corpus
  std::size_t max_failures = 20;  // examples kept for the report
};

/// Each runner returns an empty report (total == 0) when the corpus is missing.
[[nodiscard]] SuiteReport run_tokenizer_corpus(std::string_view directory, const SuiteOptions& options = {});
[[nodiscard]] SuiteReport run_tree_corpus(std::string_view directory, const SuiteOptions& options = {});
[[nodiscard]] SuiteReport run_test262_corpus(std::string_view directory, const SuiteOptions& options = {});
[[nodiscard]] SuiteReport run_reftest_corpus(std::string_view directory, const SuiteOptions& options = {});

[[nodiscard]] std::string to_markdown(const std::vector<SuiteReport>& reports);
[[nodiscard]] std::string to_json(const std::vector<SuiteReport>& reports);

/// Tiny helpers shared by the runners and the tests.
namespace detail {
[[nodiscard]] bool directory_exists(std::string_view path);
[[nodiscard]] std::vector<std::string> list_files(std::string_view directory, std::string_view suffix,
                                                  bool recursive = false);
[[nodiscard]] std::string read_file(std::string_view path);
/// HTML escapes/unescapes used by the tree-construction format.
[[nodiscard]] std::string collapse(std::string_view text);
}  // namespace detail

}  // namespace bonk::conformance
