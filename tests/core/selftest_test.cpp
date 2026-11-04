#include "test_framework.hpp"

#include "bonk/core/selftest.hpp"
#include "bonk/core/version.hpp"

using namespace bonk;

BONK_TEST(selftest, passes) {
  const SelfTestReport report = run_self_test();
  for (const std::string& failure : report.failures) {
    ::bonk::test::record_failure(__FILE__, __LINE__, "selftest: " + failure);
  }
  CHECK(report.passed());
  CHECK(report.checks.size() >= 20);
}

BONK_TEST(selftest, is_deterministic) {
  CHECK_EQ(run_self_test().checks.size(), run_self_test().checks.size());
}

BONK_TEST(version, format) {
  CHECK_EQ(version_string(), std::string("0.2.0-stage-2"));
}

BONK_TEST(version, build_string_mentions_toolchain) {
  const std::string text = build_string();
  CHECK(text.find("compiler: ") == 0);
  CHECK(text.find("c++20") != std::string::npos);
#if defined(NDEBUG)
  CHECK(text.find("release") != std::string::npos);
#else
  CHECK(text.find("debug") != std::string::npos);
#endif
}
