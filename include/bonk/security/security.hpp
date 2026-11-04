// Bonk Brauzer — the security model: origins, Same-Origin Policy, CORS, CSP
// and mixed-content rules. Pure functions, no I/O, so `bindings`, `net` and
// `shell` can all share one definition of "same origin".
//
// Implemented: tuple origins (scheme, host, port), opaque origins for `file:`,
// `Origin`/`Access-Control-Allow-*` evaluation for simple and preflighted
// requests, credentials rules, and a CSP subset (`default-src`, `script-src`,
// `style-src`, `img-src`, `connect-src`, `'self'`, `'none'`, `*`, `'unsafe-inline'`,
// scheme and host sources with a leading `*.` wildcard).
// Not implemented yet: `document.domain`, sandboxed iframes, `SameSite` cookie
// enforcement per request, CSP nonces/hashes/report-only, and redirect-based
// origin laundering checks (the redirect chain keeps the first origin for now).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bonk/net/url.hpp"

namespace bonk::security {

// ------------------------------------------------------------------ origins --

struct Origin {
  std::string scheme;
  std::string host;
  std::uint16_t port = 0;
  bool opaque = true;  // `file:` and data URLs have no usable origin

  static Origin from_url(const net::Url& url);
  /// Parses an `Origin:` header value ("https://example.com:8443" or "null").
  static Origin parse(std::string_view text);

  [[nodiscard]] std::string serialize() const;  // "null" for opaque origins
  friend bool operator==(const Origin& a, const Origin& b) noexcept {
    return a.opaque == b.opaque && a.scheme == b.scheme && a.host == b.host && a.port == b.port;
  }
  friend bool operator!=(const Origin& a, const Origin& b) noexcept { return !(a == b); }
};

[[nodiscard]] bool same_origin(const net::Url& a, const net::Url& b);
[[nodiscard]] bool is_secure_url(const net::Url& url);

/// What the URL bar should say about the connection.
enum class SecurityState { None, Insecure, Secure };
[[nodiscard]] SecurityState security_state(const net::Url& url);
[[nodiscard]] std::string_view to_string(SecurityState state) noexcept;

/// An https page must not pull in plain-http subresources.
[[nodiscard]] bool is_mixed_content(const net::Url& page, const net::Url& resource);

// --------------------------------------------------------------------- CORS --

struct CorsRequest {
  Origin origin;
  net::Url url;
  std::string method = "GET";
  std::vector<std::string> header_names;
  bool credentials = false;
};

enum class CorsVerdict { Allowed, NeedsPreflight, Blocked };

struct CorsDecision {
  CorsVerdict verdict = CorsVerdict::Blocked;
  std::string reason;
};

[[nodiscard]] bool is_simple_method(std::string_view method) noexcept;
[[nodiscard]] bool is_simple_request_header(std::string_view name) noexcept;

/// Decides, without touching the network, whether the browser may send this
/// request at all (same-origin always can, cross-origin only as a simple request
/// or after a successful preflight, which we do not perform yet).
[[nodiscard]] CorsDecision check_request(const CorsRequest& request);

struct CorsResult {
  bool allowed = false;
  std::string reason;  // "cors.allow_origin_missing", … when denied
};

/// Evaluates the response headers of a (simple) cross-origin request.
[[nodiscard]] CorsResult check_response(const Origin& requester, const net::Url& target, bool credentials,
                                        const std::vector<std::pair<std::string, std::string>>& response_headers);

// ---------------------------------------------------------------------- CSP --

class CspPolicy {
 public:
  /// Parses one `Content-Security-Policy` header/meta value. Several policies
  /// are merged (each directive's sources are unioned).
  static CspPolicy parse(std::string_view header);

  [[nodiscard]] bool empty() const noexcept { return directives_.empty(); }
  [[nodiscard]] bool allows_inline_script() const;
  [[nodiscard]] bool allows_script(std::string_view url, const Origin& self) const;
  [[nodiscard]] bool allows_style(std::string_view url, const Origin& self) const;
  [[nodiscard]] bool allows_image(std::string_view url, const Origin& self) const;
  [[nodiscard]] bool allows_connect(std::string_view url, const Origin& self) const;

  /// Human-readable summary for the devtools panel.
  [[nodiscard]] std::string describe() const;

 private:
  struct Directive {
    bool present = false;
    std::vector<std::string> sources;
  };

  [[nodiscard]] const Directive* find(std::string_view name) const;
  [[nodiscard]] bool allows(std::string_view directive, std::string_view url, const Origin& self,
                            bool inline_allowed) const;

  std::vector<std::pair<std::string, Directive>> directives_;
};

}  // namespace bonk::security
