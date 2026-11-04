#include "test_framework.hpp"

#include "bonk/net/url.hpp"

using namespace bonk;
using namespace bonk::net;

namespace {
Url parse_ok(std::string_view text, const Url* base = nullptr) {
  const Result<Url> result = parse_url(text, base);
  if (result.is_error()) {
    ::bonk::test::record_failure(__FILE__, __LINE__, "parse_url failed: " + result.error().to_string());
    return Url{};
  }
  return result.value();
}
}  // namespace

BONK_TEST(url, simple_http) {
  const Url url = parse_ok("http://example.com/path?x=1#frag");
  CHECK_EQ(url.scheme, std::string("http"));
  CHECK_EQ(url.host, std::string("example.com"));
  CHECK_EQ(url.path, std::string("/path"));
  CHECK_EQ(url.query, std::string("x=1"));
  CHECK_EQ(url.fragment, std::string("frag"));
  CHECK(!url.port.has_value());
  CHECK_EQ(url.effective_port(), 80);
}

BONK_TEST(url, explicit_port_and_default_port_omitted_in_serialization) {
  const Url url = parse_ok("https://example.com:8443/a");
  CHECK_EQ(url.effective_port(), 8443);
  CHECK_EQ(url.serialize(), std::string("https://example.com:8443/a"));
  const Url defaulted = parse_ok("http://example.com:80/a");
  CHECK_EQ(defaulted.serialize(), std::string("http://example.com/a"));
  CHECK_EQ(parse_ok("https://x/").effective_port(), 443);
}

BONK_TEST(url, uppercase_scheme_and_host_are_lowercased) {
  const Url url = parse_ok("HTTP://ExAmPle.COM/Path");
  CHECK_EQ(url.scheme, std::string("http"));
  CHECK_EQ(url.host, std::string("example.com"));
  CHECK_EQ(url.path, std::string("/Path"));
}

BONK_TEST(url, userinfo) {
  const Url url = parse_ok("http://user:pass@example.com/");
  CHECK_EQ(url.username, std::string("user"));
  CHECK_EQ(url.password, std::string("pass"));
  CHECK_EQ(url.host, std::string("example.com"));
  CHECK_EQ(url.serialize(), std::string("http://user:pass@example.com/"));
}

BONK_TEST(url, ipv4_and_ipv6_hosts) {
  CHECK_EQ(parse_ok("http://127.0.0.1:8080/x").host, std::string("127.0.0.1"));
  const Url ipv6 = parse_ok("http://[::1]:8080/x");
  CHECK_EQ(ipv6.host, std::string("[::1]"));
  CHECK_EQ(ipv6.effective_port(), 8080);
}

BONK_TEST(url, empty_path_becomes_slash_for_special_schemes) {
  CHECK_EQ(parse_ok("http://example.com").path, std::string("/"));
  CHECK_EQ(parse_ok("http://example.com").serialize(), std::string("http://example.com/"));
}

BONK_TEST(url, dot_segments_are_removed) {
  CHECK_EQ(parse_ok("http://e.com/a/b/../c").path, std::string("/a/c"));
  CHECK_EQ(parse_ok("http://e.com/a/./b/").path, std::string("/a/b/"));
  CHECK_EQ(parse_ok("http://e.com/../../x").path, std::string("/x"));
}

BONK_TEST(url, relative_references_against_base) {
  const Url base = parse_ok("http://example.com/dir/page.html?q=1");
  CHECK_EQ(parse_ok("other.html", &base).serialize(), std::string("http://example.com/dir/other.html"));
  CHECK_EQ(parse_ok("../up.html", &base).serialize(), std::string("http://example.com/up.html"));
  CHECK_EQ(parse_ok("/root.html", &base).serialize(), std::string("http://example.com/root.html"));
  CHECK_EQ(parse_ok("?x=2", &base).serialize(), std::string("http://example.com/dir/page.html?x=2"));
  CHECK_EQ(parse_ok("#top", &base).serialize(),
           std::string("http://example.com/dir/page.html?q=1#top"));  // same document, new fragment
  CHECK_EQ(parse_ok("//cdn.example.com/lib.js", &base).serialize(),
           std::string("http://cdn.example.com/lib.js"));  // query dropped, like RFC 3986
  CHECK_EQ(parse_ok("https://other.example.com/x", &base).serialize(),
           std::string("https://other.example.com/x"));
}

BONK_TEST(url, errors) {
  CHECK(parse_url("relative/only").is_error());
  CHECK(parse_url("http:///nohost").is_error());
  const Result<Url> bad_port = parse_url("http://example.com:99999/");
  REQUIRE(bad_port.is_error());
  CHECK_EQ(bad_port.error().code, std::string("url.invalid"));
  const Result<Url> idna = parse_url("http://пример.рф/");
  REQUIRE(idna.is_error());
  CHECK_EQ(idna.error().code, std::string("url.non_ascii_host"));
  CHECK(parse_url("http://example.com:abc/").is_error());
}

BONK_TEST(url, origin_and_special) {
  CHECK_EQ(parse_ok("https://example.com/a").origin(), std::string("https://example.com"));
  CHECK_EQ(parse_ok("https://example.com:443/a").origin(), std::string("https://example.com"));
  CHECK_EQ(parse_ok("https://example.com:8443/a").origin(), std::string("https://example.com:8443"));
  CHECK(parse_ok("http://x/").is_special());
  CHECK(!parse_ok("mailto:someone@example.com").is_special());
}

BONK_TEST(url, opaque_path) {
  const Url url = parse_ok("mailto:someone@example.com");
  CHECK_EQ(url.scheme, std::string("mailto"));
  CHECK_EQ(url.path, std::string("someone@example.com"));
  CHECK(url.has_opaque_path());
}

BONK_TEST(url, percent_encoding_roundtrip) {
  CHECK_EQ(percent_encode("a b/c?d"), std::string("a%20b/c?d"));
  CHECK_EQ(percent_encode("a b", false), std::string("a%20b"));
  CHECK_EQ(percent_decode("a%20b%2Fc"), std::string("a b/c"));
  CHECK_EQ(percent_decode("%zz"), std::string("%zz"));  // invalid escape stays literal
  const Url url = parse_ok("http://example.com/путь");
  CHECK_EQ(url.serialize(), std::string("http://example.com/%D0%BF%D1%83%D1%82%D1%8C"));
}
