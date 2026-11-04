#include "bonk/net/url.hpp"

#include <algorithm>
#include <cctype>

#include "bonk/core/strings.hpp"

namespace bonk::net {
namespace {

constexpr std::uint16_t kDefaultHttpPort = 80;
constexpr std::uint16_t kDefaultHttpsPort = 443;

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_hex(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hex_value(char c) noexcept {
  if (is_digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

std::optional<std::uint16_t> default_port_for(std::string_view scheme) noexcept {
  if (scheme == "http" || scheme == "ws") return kDefaultHttpPort;
  if (scheme == "https" || scheme == "wss") return kDefaultHttpsPort;
  if (scheme == "ftp") return 21;
  return std::nullopt;
}

bool is_special_scheme(std::string_view scheme) noexcept {
  return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" || scheme == "ftp" ||
         scheme == "file";
}

Error url_error(std::string_view message) { return Error::make("url.invalid", message); }

// RFC 3986 §5.2.4
std::string remove_dot_segments(std::string_view path) {
  std::string output;
  std::string_view input = path;
  while (!input.empty()) {
    if (input.rfind("../", 0) == 0) {
      input.remove_prefix(3);
    } else if (input.rfind("./", 0) == 0) {
      input.remove_prefix(2);
    } else if (input.rfind("/./", 0) == 0) {
      input.remove_prefix(2);
    } else if (input == "/.") {
      input = "/";
    } else if (input.rfind("/../", 0) == 0 || input == "/..") {
      const std::size_t last_slash = output.rfind('/');
      output.erase(last_slash == std::string::npos ? 0 : last_slash);
      input = (input == "/..") ? std::string_view("/") : input.substr(3);
    } else if (input == "." || input == "..") {
      input = {};
    } else {
      const std::size_t next = input.find('/', input[0] == '/' ? 1 : 0);
      const std::string_view segment = input.substr(0, next);
      output.append(segment);
      input.remove_prefix(segment.size());
    }
  }
  return output.empty() ? std::string("/") : output;
}

bool is_ipv4(std::string_view host) {
  int parts = 0;
  std::size_t i = 0;
  while (i < host.size() && parts < 4) {
    std::size_t start = i;
    while (i < host.size() && is_digit(host[i])) ++i;
    if (i == start || i - start > 3) return false;
    const std::string_view digits = host.substr(start, i - start);
    unsigned value = 0;
    for (const char c : digits) value = value * 10 + static_cast<unsigned>(c - '0');
    if (value > 255) return false;
    ++parts;
    if (i == host.size()) break;
    if (host[i] != '.') return false;
    ++i;
  }
  return parts == 4 && i == host.size();
}

}  // namespace

bool Url::is_special() const noexcept { return is_special_scheme(scheme); }

std::uint16_t Url::effective_port() const noexcept {
  if (port.has_value()) return *port;
  const auto fallback = default_port_for(scheme);
  return fallback.value_or(0);
}

std::string Url::origin() const {
  std::string out = scheme;
  out += "://";
  out += host;
  const auto default_port = default_port_for(scheme);
  if (port.has_value() && (!default_port.has_value() || *default_port != *port)) {
    out += ':';
    out += std::to_string(*port);
  }
  return out;
}

std::string Url::serialize(bool with_fragment) const {
  std::string out = scheme;
  out += ':';
  const bool has_authority = !host.empty() || is_special();
  if (has_authority) {
    out += "//";
    if (!username.empty()) {
      out += percent_encode(username, false);
      if (!password.empty()) {
        out += ':';
        out += percent_encode(password, false);
      }
      out += '@';
    }
    out += host;
    const auto default_port = default_port_for(scheme);
    if (port.has_value() && (!default_port.has_value() || *default_port != *port)) {
      out += ':';
      out += std::to_string(*port);
    }
  }
  out += has_opaque_path() ? path : percent_encode(path, true);
  if (!query.empty()) {
    out += '?';
    out += percent_encode(query, true);
  }
  if (with_fragment && !fragment.empty()) {
    out += '#';
    out += percent_encode(fragment, true);
  }
  return out;
}

std::string percent_decode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size() && is_hex(input[i + 1]) && is_hex(input[i + 2])) {
      out.push_back(static_cast<char>(hex_value(input[i + 1]) * 16 + hex_value(input[i + 2])));
      i += 2;
    } else {
      out.push_back(input[i]);
    }
  }
  return out;
}

std::string percent_encode(std::string_view input, bool keep_slash) {
  std::string out;
  out.reserve(input.size());
  static const char* kHex = "0123456789ABCDEF";
  for (const unsigned char c : input) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '.' || c == '_' || c == '~' || c == '!' || c == '$' || c == '\'' ||
                            c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == '=' || c == '&' ||
                            c == ':' || c == '@' || c == ';' || c == '?' || c == '[' || c == ']';
    const bool slash_ok = keep_slash && c == '/';
    if (unreserved || slash_ok) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

Result<Url> parse_url(std::string_view input, const Url* base) {
  std::string_view rest = trim(input);
  if (rest.empty()) {
    if (base != nullptr) return Result<Url>::success(*base);
    return Result<Url>::failure(url_error("empty URL"));
  }

  // A base URL inherits scheme/authority/path for relative references.
  Url url;
  bool has_scheme = false;

  const std::size_t colon = rest.find(':');
  if (colon != std::string_view::npos && colon > 0 && is_alpha(rest[0])) {
    bool valid_scheme = true;
    for (std::size_t i = 1; i < colon; ++i) {
      const char c = rest[i];
      if (!(is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.')) valid_scheme = false;
    }
    if (valid_scheme) {
      has_scheme = true;
      url.scheme = to_ascii_lower(rest.substr(0, colon));
      rest.remove_prefix(colon + 1);
    }
  }

  if (!has_scheme) {
    if (base == nullptr) return Result<Url>::failure(url_error("relative URL without a base"));
    url = *base;
    if (rest.empty()) {  // same-document reference
      url.fragment.clear();
      return Result<Url>::success(url);
    }
    if (rest[0] == '#') {
      url.fragment = percent_decode(rest.substr(1));
      return Result<Url>::success(url);
    }
    if (rest[0] == '?') {
      url.query = percent_decode(rest.substr(1));
      url.fragment.clear();
      return Result<Url>::success(url);
    }
    if (rest.rfind("//", 0) == 0) {
      // network-path reference: keeps the scheme, replaces authority, path and query
      url.query.clear();
      url.fragment.clear();
    } else if (rest[0] == '/') {
      url.path = remove_dot_segments(rest.substr(0, rest.find_first_of("?#")));
      url.query.clear();
      url.fragment.clear();
      rest = rest.substr(rest.find_first_of("?#") == std::string_view::npos ? rest.size()
                                                                           : rest.find_first_of("?#"));
      if (!rest.empty() && rest[0] == '?') {
        const std::size_t hash = rest.find('#');
        url.query = percent_decode(rest.substr(1, hash == std::string_view::npos ? std::string_view::npos
                                                                                : hash - 1));
        if (hash != std::string_view::npos) url.fragment = percent_decode(rest.substr(hash + 1));
      } else if (!rest.empty() && rest[0] == '#') {
        url.fragment = percent_decode(rest.substr(1));
      }
      return Result<Url>::success(url);
    } else {
      std::string_view path_part = rest.substr(0, rest.find_first_of("?#"));
      std::string merged = url.path;
      const std::size_t slash = merged.rfind('/');
      merged.erase(slash == std::string::npos ? 0 : slash + 1);
      merged.append(path_part);
      url.path = remove_dot_segments(merged);
      url.query.clear();
      url.fragment.clear();
      rest = rest.substr(path_part.size());
      if (!rest.empty() && rest[0] == '?') {
        const std::size_t hash = rest.find('#');
        url.query = percent_decode(rest.substr(1, hash == std::string_view::npos ? std::string_view::npos
                                                                                : hash - 1));
        if (hash != std::string_view::npos) url.fragment = percent_decode(rest.substr(hash + 1));
      } else if (!rest.empty() && rest[0] == '#') {
        url.fragment = percent_decode(rest.substr(1));
      }
      return Result<Url>::success(url);
    }
  }

  // Absolute reference: split off fragment and query.
  const std::size_t hash = rest.find('#');
  if (hash != std::string_view::npos) {
    url.fragment = percent_decode(rest.substr(hash + 1));
    rest = rest.substr(0, hash);
  }
  const std::size_t question = rest.find('?');
  if (question != std::string_view::npos) {
    url.query = percent_decode(rest.substr(question + 1));
    rest = rest.substr(0, question);
  }

  if (rest.rfind("//", 0) == 0) {
    rest.remove_prefix(2);
    const std::size_t authority_end = rest.find('/');
    std::string_view authority = rest.substr(0, authority_end);
    rest = (authority_end == std::string_view::npos) ? std::string_view{} : rest.substr(authority_end);

    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
      std::string_view userinfo = authority.substr(0, at);
      const std::size_t user_colon = userinfo.find(':');
      if (user_colon == std::string_view::npos) {
        url.username = percent_decode(userinfo);
      } else {
        url.username = percent_decode(userinfo.substr(0, user_colon));
        url.password = percent_decode(userinfo.substr(user_colon + 1));
      }
      authority = authority.substr(at + 1);
    }

    std::string_view host_part = authority;
    std::string_view port_part;
    if (!authority.empty() && authority[0] == '[') {
      const std::size_t close = authority.find(']');
      if (close == std::string_view::npos) return Result<Url>::failure(url_error("unterminated IPv6 host"));
      host_part = authority.substr(0, close + 1);
      if (close + 1 < authority.size() && authority[close + 1] == ':') port_part = authority.substr(close + 2);
    } else {
      const std::size_t port_colon = authority.rfind(':');
      if (port_colon != std::string_view::npos) {
        host_part = authority.substr(0, port_colon);
        port_part = authority.substr(port_colon + 1);
      }
    }

    if (!port_part.empty()) {
      unsigned long long value = 0;
      if (!parse_u64(port_part, value) || value > 65535) {
        return Result<Url>::failure(url_error("invalid port '" + std::string(port_part) + "'"));
      }
      url.port = static_cast<std::uint16_t>(value);
    }

    if (host_part.empty() && is_special_scheme(url.scheme)) {
      return Result<Url>::failure(url_error("missing host for special scheme '" + url.scheme + "'"));
    }
    if (url.scheme == "file") {
      url.host.clear();
      url.host.append(host_part);
    } else if (!host_part.empty() && host_part[0] != '[') {
      for (const char c : host_part) {
        if (static_cast<unsigned char>(c) >= 0x80) {
          return Result<Url>::failure(Error::make("url.non_ascii_host",
                                                  "IDNA/punycode is not implemented; use an ASCII or punycode host"));
        }
      }
      url.host = to_ascii_lower(host_part);
      if (!is_ipv4(url.host) && url.host.find('.') == std::string::npos && url.host != "localhost") {
        // A single-label host is legal (intranet), only a warning-worthy oddity later.
      }
    } else {
      url.host = to_ascii_lower(host_part);
    }

    url.path = remove_dot_segments(rest.empty() ? std::string_view("/") : rest);
    return Result<Url>::success(url);
  }

  // No authority: either file-like ("/etc/hosts") or an opaque path ("mailto:x@y").
  if (is_special_scheme(url.scheme)) {
    if (rest.empty() || rest[0] != '/') {
      return Result<Url>::failure(url_error("special scheme needs an absolute path"));
    }
    url.path = remove_dot_segments(rest);
    return Result<Url>::success(url);
  }

  url.path = rest.empty() ? std::string() : std::string(rest);
  return Result<Url>::success(url);
}

}  // namespace bonk::net
