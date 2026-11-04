#include <string>

#include "test_framework.hpp"

#include "bonk/core/result.hpp"

using namespace bonk;

namespace {
Result<int> halve(int value) {
  if (value % 2 != 0) return Result<int>::failure(Error::make("math.odd", "value is odd"));
  return Result<int>::success(value / 2);
}

Status check_positive(int value) {
  if (value <= 0) return Status::failure(Error::make("math.non_positive", "must be positive"));
  return Status::success();
}
}  // namespace

BONK_TEST(result, success_carries_value) {
  const Result<int> result = halve(10);
  CHECK(result.is_ok());
  CHECK(!result.is_error());
  CHECK(static_cast<bool>(result));
  CHECK_EQ(result.value(), 5);
}

BONK_TEST(result, failure_carries_error) {
  const Result<int> result = halve(7);
  CHECK(result.is_error());
  CHECK_EQ(result.error().code, std::string("math.odd"));
  CHECK_EQ(result.value_or(42), 42);
}

BONK_TEST(result, string_payload) {
  const Result<std::string> result = Result<std::string>::success("ok");
  CHECK_EQ(result.value(), std::string("ok"));
}

BONK_TEST(result, status_conversion) {
  CHECK(halve(4).status().is_ok());
  Status failed = halve(3).status();
  CHECK(failed.is_error());
  CHECK_EQ(failed.error().code, std::string("math.odd"));
  CHECK(failed.take_error().has_value());
  CHECK(!failed.take_error().has_value());  // moved out
}

BONK_TEST(result, status_type) {
  CHECK(check_positive(1).is_ok());
  const Status bad = check_positive(0);
  CHECK(bad.is_error());
  CHECK_EQ(bad.error().message, std::string("must be positive"));
}

BONK_TEST(error, formatting) {
  const Error plain = Error::make("net.dns_failure", "no A record");
  CHECK_EQ(plain.to_string(), std::string("net.dns_failure: no A record"));

  const Error located = Error::make("url.invalid_scheme", "expected http or https", "https://x/", 3, 7);
  CHECK_EQ(located.to_string(), std::string("url.invalid_scheme: expected http or https (at https://x/:3:7)"));

  Error chained = Error::make("css.bad_selector", "expected identifier");
  chained.located_at("page.css", 12, 0);
  CHECK_EQ(chained.to_string(), std::string("css.bad_selector: expected identifier (at page.css:12)"));
}
