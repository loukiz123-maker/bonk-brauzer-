// Minimal test framework (≈180 lines) instead of gtest/catch2.
//
// Why not a dependency: the project rule is "standard library only", and the
// whole feature set we need is auto-registration, CHECK/REQUIRE, value printing
// and a filter flag. Self-contained means `make test` works on a bare machine
// with no network and no package manager.
#pragma once

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bonk::test {

struct TestCase {
  std::string_view suite;
  std::string_view name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline std::vector<std::string>& current_failures() {
  static std::vector<std::string> failures;
  return failures;
}

struct Registrar {
  Registrar(std::string_view suite, std::string_view name, void (*fn)()) { registry().push_back({suite, name, fn}); }
};

// --- value printing ------------------------------------------------------
inline std::string show(std::string_view value) { return std::string("\"") + std::string(value) + "\""; }

template <class T>
std::string show(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, char>) {
    return std::string("'") + value + "'";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_floating_point_v<T>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    return show(std::string_view(value));
  } else {
    return "<unprintable>";
  }
}

// --- failure recording ---------------------------------------------------
inline void record_failure(std::string_view file, int line, std::string message) {
  std::string frame = std::string(file) + ":" + std::to_string(line) + ": " + std::move(message);
  current_failures().push_back(std::move(frame));
}

// --- runner --------------------------------------------------------------
inline int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--list") {
      list_only = true;
    } else if (arg.rfind("--filter=", 0) == 0) {
      filter = std::string(arg.substr(9));
    } else {
      std::fprintf(stderr, "bonk_tests: unknown argument '%s'\n", argv[i]);
      return 2;
    }
  }

  std::size_t run = 0;
  std::size_t failed = 0;
  for (const TestCase& test : registry()) {
    const std::string full = std::string(test.suite) + "." + std::string(test.name);
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    if (list_only) {
      std::printf("%s\n", full.c_str());
      continue;
    }

    current_failures().clear();
    test.fn();
    ++run;

    if (current_failures().empty()) {
      std::printf("[  OK  ] %s\n", full.c_str());
    } else {
      ++failed;
      std::printf("[ FAIL ] %s\n", full.c_str());
      for (const std::string& failure : current_failures()) {
        std::printf("         %s\n", failure.c_str());
      }
    }
  }

  if (list_only) return 0;
  std::printf("\n%zu test(s) run, %zu failed\n", run, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace bonk::test

#define BONK_TEST(suite, name)                                                              \
  static void bonk_test_##suite##_##name();                                                 \
  static ::bonk::test::Registrar bonk_registrar_##suite##_##name(#suite, #name,             \
                                                                &bonk_test_##suite##_##name); \
  static void bonk_test_##suite##_##name()

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) ::bonk::test::record_failure(__FILE__, __LINE__, "CHECK failed: " #condition); \
  } while (false)

#define CHECK_EQ(lhs, rhs)                                                                    \
  do {                                                                                        \
    const auto& bonk_lhs = (lhs);                                                             \
    const auto& bonk_rhs = (rhs);                                                             \
    if (!(bonk_lhs == bonk_rhs)) {                                                            \
      ::bonk::test::record_failure(__FILE__, __LINE__,                                        \
                                   std::string("CHECK_EQ failed: " #lhs " == " #rhs " | left=") + \
                                       ::bonk::test::show(bonk_lhs) + " right=" +             \
                                       ::bonk::test::show(bonk_rhs));                         \
    }                                                                                         \
  } while (false)

#define CHECK_NE(lhs, rhs)                                                                    \
  do {                                                                                        \
    const auto& bonk_lhs = (lhs);                                                             \
    const auto& bonk_rhs = (rhs);                                                             \
    if (bonk_lhs == bonk_rhs) {                                                               \
      ::bonk::test::record_failure(__FILE__, __LINE__,                                        \
                                   std::string("CHECK_NE failed: " #lhs " != " #rhs " | both=") + \
                                       ::bonk::test::show(bonk_lhs));                         \
    }                                                                                         \
  } while (false)

// REQUIRE aborts the current test (records the failure, then returns).
#define REQUIRE(condition)                                                                    \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      ::bonk::test::record_failure(__FILE__, __LINE__, "REQUIRE failed: " #condition);         \
      return;                                                                                   \
    }                                                                                         \
  } while (false)
