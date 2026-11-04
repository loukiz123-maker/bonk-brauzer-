// Bonk Brauzer — URL parsing and resolution (subset of WHATWG URL, "special" schemes).
//
// Implemented: http, https, ws, wss, ftp, file + generic "unknown scheme" form,
// userinfo, IPv4/IPv6 hosts, default ports, percent-encoding of the path/query,
// RFC 3986 "remove dot segments" and relative reference resolution against a base.
// Not implemented yet (documented, not stubbed): IDNA/punycode (non-ASCII hosts
// are rejected with a diagnostic instead of silently mangled), data:/blob: URLs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "bonk/core/result.hpp"

namespace bonk::net {

struct Url {
  std::string scheme;   // lowercase, without ':'
  std::string username;
  std::string password;
  std::string host;     // lowercase; IPv6 kept bracketed as "[::1]"
  std::optional<std::uint16_t> port;  // explicit port only; default port stays nullopt
  std::string path;     // always starts with '/' for special schemes, never empty there
  std::string query;    // without '?'
  std::string fragment; // without '#'

  [[nodiscard]] bool has_opaque_path() const noexcept { return path.empty() || path[0] != '/'; }
  [[nodiscard]] std::uint16_t effective_port() const noexcept;
  [[nodiscard]] std::string serialize(bool with_fragment = true) const;
  [[nodiscard]] std::string origin() const;  // "scheme://host[:port]" (used by SOP later)
  [[nodiscard]] bool is_special() const noexcept;
};

/// Parse an absolute URL. Relative input without `base` is an error ("url.relative_without_base").
[[nodiscard]] Result<Url> parse_url(std::string_view input, const Url* base = nullptr);

/// percent-encode / decode. `encode` keeps unreserved + sub-delims that are legal in a path.
[[nodiscard]] std::string percent_decode(std::string_view input);
[[nodiscard]] std::string percent_encode(std::string_view input, bool keep_slash = true);

}  // namespace bonk::net
