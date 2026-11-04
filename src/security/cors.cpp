#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "bonk/core/strings.hpp"
#include "bonk/security/security.hpp"

namespace bonk::security {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

const std::string* find_header(const std::vector<std::pair<std::string, std::string>>& headers,
                               std::string_view name) {
  for (const auto& [key, value] : headers) {
    if (lower(key) == name) return &value;
  }
  return nullptr;
}

}  // namespace

bool is_simple_method(std::string_view method) noexcept {
  const std::string lowered = lower(method);
  return lowered == "get" || lowered == "head" || lowered == "post";
}

bool is_simple_request_header(std::string_view name) noexcept {
  const std::string lowered = lower(name);
  return lowered == "accept" || lowered == "accept-language" || lowered == "content-language" ||
         lowered == "content-type" || lowered == "range";
}

CorsDecision check_request(const CorsRequest& request) {
  const Origin target = Origin::from_url(request.url);
  if (request.origin.opaque) {
    // Opaque origins (file:, sandboxed) may only read same-scheme local resources;
    // every network read is blocked, which is what browsers do for `fetch` from
    // a file: page.
    return CorsDecision{CorsVerdict::Blocked, "cors.opaque_origin"};
  }
  if (request.origin == target) return CorsDecision{CorsVerdict::Allowed, {}};

  if (!is_simple_method(request.method)) {
    return CorsDecision{CorsVerdict::NeedsPreflight, "cors.preflight_required_method"};
  }
  for (const std::string& name : request.header_names) {
    if (is_simple_request_header(name)) continue;
    return CorsDecision{CorsVerdict::NeedsPreflight, "cors.preflight_required_header"};
  }
  return CorsDecision{CorsVerdict::Allowed, {}};
}

CorsResult check_response(const Origin& requester, const net::Url& target, bool credentials,
                          const std::vector<std::pair<std::string, std::string>>& response_headers) {
  if (requester == Origin::from_url(target)) return CorsResult{true, {}};
  if (requester.opaque) return CorsResult{false, "cors.opaque_origin"};

  const std::string* allow_origin = find_header(response_headers, "access-control-allow-origin");
  if (allow_origin == nullptr) {
    return CorsResult{false, "cors.allow_origin_missing"};
  }
  const std::string_view value = trim(*allow_origin);
  if (value == "*") {
    // `*` never authorises credentialed reads.
    if (credentials) return CorsResult{false, "cors.wildcard_with_credentials"};
    return CorsResult{true, {}};
  }
  if (Origin::parse(value) != requester) {
    return CorsResult{false, "cors.allow_origin_mismatch"};
  }
  if (credentials) {
    const std::string* allow_credentials = find_header(response_headers, "access-control-allow-credentials");
    if (allow_credentials == nullptr || lower(trim(*allow_credentials)) != "true") {
      return CorsResult{false, "cors.allow_credentials_missing"};
    }
  }
  return CorsResult{true, {}};
}

}  // namespace bonk::security
