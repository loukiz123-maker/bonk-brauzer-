#include <string>

#include "bonk/security/security.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace bonk::security;

namespace {

net::Url url_of(std::string_view text) {
  const Result<net::Url> url = net::parse_url(text);
  if (url.is_error()) ::bonk::test::record_failure(__FILE__, __LINE__, "bad url: " + std::string(text));
  return url.value();
}

std::vector<std::pair<std::string, std::string>> headers(std::initializer_list<std::pair<const char*, const char*>> items) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [name, value] : items) out.emplace_back(name, value);
  return out;
}

}  // namespace

BONK_TEST(security, origin_tuples_and_serialization) {
  const Origin https = Origin::from_url(url_of("https://example.com:443/a"));
  const Origin plain = Origin::from_url(url_of("http://example.com/a"));
  CHECK_EQ(https.serialize(), std::string("https://example.com"));
  CHECK_EQ(plain.serialize(), std::string("http://example.com"));
  CHECK(https != plain);  // scheme is part of the tuple
  CHECK_EQ(Origin::from_url(url_of("https://example.com:8443/")).serialize(),
           std::string("https://example.com:8443"));
  const Origin file = Origin::from_url(url_of("file:/tmp/a.html"));
  CHECK(file.opaque);
  CHECK_EQ(file.serialize(), std::string("null"));
  CHECK_EQ(Origin::parse("null").opaque, true);
  CHECK_EQ(Origin::parse("https://Example.com:8443").serialize(), std::string("https://example.com:8443"));
}

BONK_TEST(security, same_origin_rules) {
  CHECK(same_origin(url_of("https://a.test/x"), url_of("https://a.test/y")));
  CHECK(!same_origin(url_of("https://a.test/x"), url_of("http://a.test/x")));
  CHECK(!same_origin(url_of("https://a.test/x"), url_of("https://b.test/x")));
  CHECK(!same_origin(url_of("https://a.test/x"), url_of("https://a.test:8443/x")));
  // Local files have opaque origins: never same-origin, not even with themselves.
  CHECK(!same_origin(url_of("file:/tmp/a.html"), url_of("file:/tmp/a.html")));
}

BONK_TEST(security, security_state_and_mixed_content) {
  CHECK(security_state(url_of("https://a.test/")) == SecurityState::Secure);
  CHECK(security_state(url_of("http://a.test/")) == SecurityState::Insecure);
  CHECK(security_state(url_of("file:/tmp/a.html")) == SecurityState::None);
  CHECK_EQ(std::string(to_string(SecurityState::Secure)), std::string("secure"));

  CHECK(is_mixed_content(url_of("https://a.test/"), url_of("http://cdn.test/x.png")));
  CHECK(!is_mixed_content(url_of("https://a.test/"), url_of("https://cdn.test/x.png")));
  CHECK(!is_mixed_content(url_of("http://a.test/"), url_of("http://cdn.test/x.png")));
}

BONK_TEST(security, cors_request_classification) {
  const Origin page = Origin::from_url(url_of("https://app.test/page"));
  CorsRequest same;
  same.origin = page;
  same.url = url_of("https://app.test/api");
  CHECK(check_request(same).verdict == CorsVerdict::Allowed);

  CorsRequest simple;
  simple.origin = page;
  simple.url = url_of("https://api.test/items");
  simple.method = "GET";
  simple.header_names = {"accept", "content-type"};
  CHECK(check_request(simple).verdict == CorsVerdict::Allowed);

  CorsRequest put = simple;
  put.method = "PUT";
  CHECK(check_request(put).verdict == CorsVerdict::NeedsPreflight);

  CorsRequest custom = simple;
  custom.header_names = {"x-api-key"};
  const CorsDecision decision = check_request(custom);
  CHECK(decision.verdict == CorsVerdict::NeedsPreflight);
  CHECK_EQ(decision.reason, std::string("cors.preflight_required_header"));

  CorsRequest local;
  local.origin = Origin::from_url(url_of("file:/tmp/a.html"));
  local.url = url_of("https://api.test/items");
  CHECK(check_request(local).verdict == CorsVerdict::Blocked);
  CHECK_EQ(check_request(local).reason, std::string("cors.opaque_origin"));
}

BONK_TEST(security, cors_response_evaluation) {
  const Origin page = Origin::from_url(url_of("https://app.test/page"));
  const net::Url target = url_of("https://api.test/items");

  CHECK(check_response(page, url_of("https://app.test/other"), false, {}).allowed);  // same origin
  CHECK(check_response(page, target, false, headers({{"Access-Control-Allow-Origin", "*"}})).allowed);
  CHECK(!check_response(page, target, true, headers({{"Access-Control-Allow-Origin", "*"}})).allowed);
  CHECK(check_response(page, target, false, headers({{"access-control-allow-origin", "https://app.test"}})).allowed);
  CHECK(!check_response(page, target, false, headers({{"Access-Control-Allow-Origin", "https://evil.test"}})).allowed);
  CHECK_EQ(check_response(page, target, false, {}).reason, std::string("cors.allow_origin_missing"));
  // Credentialed reads need an exact origin plus ACAC: true.
  const auto credentialed = headers({{"Access-Control-Allow-Origin", "https://app.test"},
                                     {"Access-Control-Allow-Credentials", "true"}});
  CHECK(check_response(page, target, true, credentialed).allowed);
  const auto missing_acac = headers({{"Access-Control-Allow-Origin", "https://app.test"}});
  CHECK_EQ(check_response(page, target, true, missing_acac).reason, std::string("cors.allow_credentials_missing"));
}

BONK_TEST(security, csp_inline_scripts_and_sources) {
  const Origin self = Origin::from_url(url_of("https://app.test/page"));
  CHECK(CspPolicy::parse("").allows_inline_script());  // no policy: everything allowed
  CHECK(!CspPolicy::parse("script-src 'self'").allows_inline_script());
  CHECK(CspPolicy::parse("script-src 'self' 'unsafe-inline'").allows_inline_script());
  CHECK(CspPolicy::parse("default-src https:").allows_script("https://cdn.test/a.js", self));
  CHECK(!CspPolicy::parse("default-src https:").allows_script("http://cdn.test/a.js", self));
  CHECK(CspPolicy::parse("img-src *.cdn.test").allows_image("https://static.cdn.test/x.png", self));
  CHECK(!CspPolicy::parse("img-src *.cdn.test").allows_image("https://cdn.test/x.png", self));
  CHECK(CspPolicy::parse("img-src 'self'").allows_image("https://app.test/x.png", self));
  CHECK(!CspPolicy::parse("img-src 'none'").allows_image("https://app.test/x.png", self));
  CHECK(CspPolicy::parse("connect-src 'self' https://api.test").allows_connect("https://api.test/items", self));
  CHECK(!CspPolicy::parse("connect-src 'self'").allows_connect("https://api.test/items", self));
  // An unrelated directive must not restrict another kind of fetch.
  CHECK(CspPolicy::parse("style-src 'none'").allows_script("https://cdn.test/a.js", self));
  CHECK_EQ(CspPolicy::parse("default-src 'self'; img-src *").describe().empty(), false);
}
