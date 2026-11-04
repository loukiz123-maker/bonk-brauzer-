// Bonk Brauzer — HTTP cookies (RFC 6265, the parts a browser notices).
//
// Implemented: name=value pairs, Domain (with host-only vs domain cookies),
// Path, Expires, Max-Age, Secure, HttpOnly (hidden from document.cookie),
// default path computation, longest-path-first ordering, deletion by expiry.
// Not implemented: Public Suffix List enforcement (we accept a domain cookie for
// any suffix of the host, so `evil.example.com` cannot set `.com` but a page on
// `example.com` can set `.com`'s sibling domains only by exact match), and
// SameSite (stage 6, with the SOP work).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/net/url.hpp"

namespace bonk::net {

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;      // lowercase, without a leading dot
  std::string path = "/";
  bool host_only = true;   // set without an explicit Domain attribute
  bool secure = false;
  bool http_only = false;
  std::int64_t expires_at = 0;  // unix seconds; 0 = session cookie

  [[nodiscard]] bool expired(std::int64_t now) const noexcept {
    return expires_at != 0 && expires_at <= now;
  }
};

class CookieJar {
 public:
  /// Parses a `Set-Cookie` header received for `url` and stores/deletes the cookie.
  /// Returns false (and stores nothing) when the header is malformed or rejected.
  bool store_from_header(const Url& url, std::string_view header);

  /// Value for the `Cookie:` request header ("a=1; b=2"), or an empty string.
  [[nodiscard]] std::string cookie_header(const Url& url, std::int64_t now = 0) const;

  /// `document.cookie` view: non-HttpOnly cookies, path/domain filtered.
  [[nodiscard]] std::string document_cookie(const Url& url, std::int64_t now = 0) const;

  /// `document.cookie = "a=1; path=/; max-age=60"`.
  void set_document_cookie(const Url& url, std::string_view text);

  [[nodiscard]] const std::vector<Cookie>& cookies() const noexcept { return cookies_; }
  void clear() { cookies_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return cookies_.size(); }

  /// Drops expired cookies (called by the accessors, exposed for tests).
  void purge(std::int64_t now = 0);

 private:
  std::vector<Cookie> cookies_;
};

/// Current UTC time in unix seconds.
[[nodiscard]] std::int64_t current_unix_time() noexcept;

/// Parses an `Expires` cookie date ("Wdy, DD Mon YYYY HH:MM:SS GMT").
/// Returns 0 when the value cannot be understood.
[[nodiscard]] std::int64_t parse_cookie_date(std::string_view text) noexcept;

/// Default cookie path for a request URL (RFC 6265 §5.1.4).
[[nodiscard]] std::string default_cookie_path(const Url& url);

}  // namespace bonk::net
