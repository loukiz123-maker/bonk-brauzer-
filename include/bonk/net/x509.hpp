// Bonk Brauzer — minimal X.509 v3 parsing for TLS.
//
// We parse what a browser needs to make a trust decision it can explain:
// subject CN, subjectAltName dNSName entries, validity window, serial, issuer.
// Signature verification (RSA-PSS / ECDSA) and chain building are stage 6 work —
// see docs/STAGE-02B.md for the exact gap and why it is visible at runtime.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk::net {

struct Certificate {
  std::string subject_common_name;
  std::string issuer_common_name;
  std::vector<std::string> dns_names;  // subjectAltName dNSName entries
  std::string serial_hex;
  std::string not_before;  // "YYYY-MM-DDTHH:MM:SSZ" (UTC)
  std::string not_after;
};

/// Parses a DER-encoded X.509 certificate. Errors: "x509.*".
[[nodiscard]] Result<Certificate> parse_certificate(std::string_view der);

/// RFC 6125-style match: exact, or a single-label wildcard ("*.example.com").
[[nodiscard]] bool certificate_matches_host(const Certificate& certificate, std::string_view host);

/// True when `now_iso8601` (default: current UTC time) is inside [not_before, not_after).
[[nodiscard]] bool certificate_is_valid_at(const Certificate& certificate, std::string_view now_iso8601 = {});

/// Current UTC time formatted like the certificate timestamps.
[[nodiscard]] std::string current_utc_timestamp();

}  // namespace bonk::net
