// Bonk Brauzer — HTTP/1.1 client.
//
// Implemented: DNS via getaddrinfo, TCP with connect/send/receive timeouts,
// request serialisation, status line + header parsing, Content-Length bodies,
// chunked transfer decoding, `Connection: close`, redirect following (301/302/
// 303/307/308, max 5 hops, relative Location resolution).
// Not implemented yet (explicit errors, never silent hangs): HTTPS (TLS is the
// next net milestone), gzip/deflate content encoding (we ask for `identity`),
// keep-alive connection pooling, HTTP/2.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bonk/core/diag.hpp"
#include "bonk/core/result.hpp"
#include "bonk/net/url.hpp"

namespace bonk::net {

struct HttpRequest {
  Url url;
  std::string method = "GET";
  std::vector<std::pair<std::string, std::string>> headers;
  int timeout_ms = 5000;
  int max_redirects = 5;
};

struct HttpResponse {
  int status = 0;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string final_url;
  int redirect_count = 0;

  [[nodiscard]] std::string_view header(std::string_view name) const;
};

/// Performs the request (following redirects). Errors: "net.*".
[[nodiscard]] Result<HttpResponse> http_get(const Url& url, Diagnostics* diagnostics = nullptr,
                                            int timeout_ms = 5000);

/// Full control version, used by tests and by `http_get`.
[[nodiscard]] Result<HttpResponse> http_request(const HttpRequest& request, Diagnostics* diagnostics = nullptr);

/// Raw HTTP body decoding helpers (exposed for unit tests).
[[nodiscard]] Result<std::string> decode_chunked(std::string_view body);
/// Content-Encoding decoders built on our own inflate (src/net/gzip.cpp).
[[nodiscard]] Result<std::string> decode_gzip(std::string_view body);
[[nodiscard]] Result<std::string> decode_zlib(std::string_view body);

}  // namespace bonk::net
