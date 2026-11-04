// Bonk Brauzer — runtime self test.
//
// Unit tests (`make test`) verify behaviour in depth; the self test verifies
// that the *shipped binary* is internally consistent (correct UTF-8 tables,
// working arena, complete module registry). It runs in microseconds and is
// what `bonk selftest` and the smoke job in CI call.
#pragma once

#include <string>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk {

struct SelfTestReport {
  std::vector<std::string> checks;      // "utf8.roundtrip: ok"
  std::vector<std::string> failures;    // "arena.alignment: expected ... got ..."
  [[nodiscard]] bool passed() const noexcept { return failures.empty(); }
};

[[nodiscard]] SelfTestReport run_self_test();

}  // namespace bonk
