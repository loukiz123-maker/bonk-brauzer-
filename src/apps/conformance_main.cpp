// `bonk_conformance` — the conformance harness executable (stage 7).
//
//   bonk_conformance --corpus DIR [--suite tokenizer|tree|test262|reftests|all]
//                    [--limit N] [--max-failures N] [--json] [--markdown] [--quiet]
//
// Corpus layout expected under --corpus (see tools/fetch_conformance.sh):
//   tokenizer/     html5lib-tests tokenizer/*.test
//   tree/          html5lib-tests tree-construction/*.dat
//   test262/test/  test262 test/ directory
//   reftests/      our own <name>.html + <name>-ref.html pairs (in-repo)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bonk/conformance/conformance.hpp"
#include "bonk/core/version.hpp"

namespace {

struct Options {
  std::string corpus = "tests/conformance/corpus";
  std::string suite = "all";
  std::size_t limit = 0;
  std::size_t max_failures = 15;
  std::string reftests;
  bool json = false;
  bool markdown = false;
  bool quiet = false;
};

void print_usage() {
  std::printf(
      "usage: bonk_conformance [options]\n"
      "  --corpus DIR        corpus root (default tests/conformance/corpus)\n"
      "  --suite NAME        all | tokenizer | tree | test262 | reftests\n"
      "  --reftests DIR      override the reftest directory (default <corpus>/reftests)\n"
      "  --limit N           stop after N cases per suite (0 = everything)\n"
      "  --max-failures N    examples kept per suite (default 15)\n"
      "  --json              machine-readable report\n"
      "  --markdown          report as markdown tables\n"
      "  --quiet             only the summary table\n"
      "  --version           print the version and exit\n");
}

[[nodiscard]] bool wants(const std::string& suite, const char* name) {
  return suite == "all" || suite == name;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "bonk_conformance: %s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (argument == "--corpus") {
      options.corpus = next("--corpus");
    } else if (argument == "--suite") {
      options.suite = next("--suite");
    } else if (argument == "--reftests") {
      options.reftests = next("--reftests");
    } else if (argument == "--limit") {
      options.limit = static_cast<std::size_t>(std::strtoul(next("--limit").c_str(), nullptr, 10));
    } else if (argument == "--max-failures") {
      options.max_failures = static_cast<std::size_t>(std::strtoul(next("--max-failures").c_str(), nullptr, 10));
    } else if (argument == "--json") {
      options.json = true;
    } else if (argument == "--markdown") {
      options.markdown = true;
    } else if (argument == "--quiet") {
      options.quiet = true;
    } else if (argument == "--version") {
      std::printf("%s\n", bonk::version_string().c_str());
      return 0;
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "bonk_conformance: unknown option '%s' (try --help)\n", argument.c_str());
      return 2;
    }
  }

  bonk::conformance::SuiteOptions suite_options;
  suite_options.limit = options.limit;
  suite_options.max_failures = options.max_failures;

  std::vector<bonk::conformance::SuiteReport> reports;
  if (wants(options.suite, "tokenizer")) {
    reports.push_back(bonk::conformance::run_tokenizer_corpus(options.corpus + "/tokenizer", suite_options));
  }
  if (wants(options.suite, "tree")) {
    reports.push_back(bonk::conformance::run_tree_corpus(options.corpus + "/tree", suite_options));
  }
  if (wants(options.suite, "test262")) {
    reports.push_back(bonk::conformance::run_test262_corpus(options.corpus + "/test262/test", suite_options));
  }
  if (wants(options.suite, "reftests")) {
    const std::string directory = options.reftests.empty() ? options.corpus + "/reftests" : options.reftests;
    reports.push_back(bonk::conformance::run_reftest_corpus(directory, suite_options));
  }

  if (options.json) {
    std::fputs(bonk::conformance::to_json(reports).c_str(), stdout);
    return 0;
  }

  if (options.markdown) {
    std::fputs(bonk::conformance::to_markdown(reports).c_str(), stdout);
    return 0;
  }

  std::printf("%-26s %8s %8s %8s %8s %8s %8s\n", "suite", "cases", "passed", "failed", "skipped", "excluded",
              "rate");
  for (const bonk::conformance::SuiteReport& report : reports) {
    if (report.total == 0 && report.skipped == 0) {
      std::printf("%-26s %8s %8s %8s %8s %8s %8s\n", report.suite.c_str(), "-", "-", "-", "-", "-", "missing");
      continue;
    }
    std::printf("%-26s %8zu %8zu %8zu %8zu %8zu %7.1f%%\n", report.suite.c_str(), report.total, report.passed,
                report.failed(), report.skipped, report.excluded, report.pass_rate());
    if (options.quiet) continue;
    for (const std::string& note : report.notes) std::printf("    note: %s\n", note.c_str());
    for (const bonk::conformance::CaseFailure& failure : report.failures) {
      std::printf("    - %s :: %s\n", failure.name.c_str(), failure.detail.c_str());
    }
  }
  return 0;
}
