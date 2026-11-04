#include <cctype>
#include <string>

#include "bonk/core/strings.hpp"
#include "bonk/security/security.hpp"

namespace bonk::security {

Origin Origin::from_url(const net::Url& url) {
  Origin origin;
  if (url.scheme == "file" || url.scheme.empty()) {
    // Local files are treated as opaque: no script-visible origin, no storage.
    return origin;
  }
  origin.scheme = url.scheme;
  origin.host = url.host;
  origin.port = url.effective_port();
  origin.opaque = url.host.empty();
  return origin;
}

Origin Origin::parse(std::string_view text) {
  Origin origin;
  const std::string_view trimmed = trim(text);
  if (trimmed.empty() || trimmed == "null") return origin;
  const Result<net::Url> url = net::parse_url(trimmed);
  if (url.is_error()) return origin;
  return from_url(url.value());
}

std::string Origin::serialize() const {
  if (opaque || scheme.empty() || host.empty()) return "null";
  std::string out = scheme;
  out += "://";
  out += host;
  const std::uint16_t default_port = scheme == "https" || scheme == "wss" ? 443 : 80;
  if (port != 0 && port != default_port) {
    out += ':';
    out += std::to_string(port);
  }
  return out;
}

bool same_origin(const net::Url& a, const net::Url& b) {
  const Origin left = Origin::from_url(a);
  const Origin right = Origin::from_url(b);
  if (left.opaque || right.opaque) return false;  // opaque origins are only same-origin with themselves
  return left == right;
}

bool is_secure_url(const net::Url& url) { return url.scheme == "https" || url.scheme == "wss"; }

SecurityState security_state(const net::Url& url) {
  if (url.scheme.empty() || url.scheme == "file") return SecurityState::None;
  return is_secure_url(url) ? SecurityState::Secure : SecurityState::Insecure;
}

std::string_view to_string(SecurityState state) noexcept {
  switch (state) {
    case SecurityState::None:
      return "none";
    case SecurityState::Insecure:
      return "insecure";
    case SecurityState::Secure:
      return "secure";
  }
  return "none";
}

bool is_mixed_content(const net::Url& page, const net::Url& resource) {
  if (!is_secure_url(page)) return false;
  if (resource.scheme != "http") return false;  // data:, blob:, https: are fine
  return true;
}

}  // namespace bonk::security
