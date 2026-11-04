#include "bonk/net/cookie.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include "bonk/core/strings.hpp"

namespace bonk::net {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool is_ip_address(std::string_view host) {
  if (host.empty()) return false;
  if (host.front() == '[') return true;  // IPv6 literal
  int dots = 0;
  for (const char c : host) {
    if (c == '.') ++dots;
    else if (c < '0' || c > '9') return false;
  }
  return dots == 3;
}

/// RFC 6265 §5.1.3 domain matching.
bool domain_matches(std::string_view host, std::string_view domain) {
  if (host == domain) return true;
  if (host.size() > domain.size() && host.ends_with(domain)) {
    const std::size_t prefix_end = host.size() - domain.size() - 1;
    if (host[prefix_end] == '.') return true;
  }
  return false;
}

/// RFC 6265 §5.1.4 path matching.
bool path_matches(std::string_view request_path, std::string_view cookie_path) {
  if (cookie_path.empty()) return true;
  if (request_path == cookie_path) return true;
  if (!request_path.starts_with(cookie_path)) return false;
  return cookie_path.back() == '/' || (request_path.size() > cookie_path.size() && request_path[cookie_path.size()] == '/');
}

std::string host_of(const Url& url) { return lower(url.host); }

std::string path_of(const Url& url) { return url.path.empty() ? std::string("/") : url.path; }

std::int64_t now_or_current(std::int64_t now) { return now != 0 ? now : current_unix_time(); }

}  // namespace

std::int64_t current_unix_time() noexcept { return static_cast<std::int64_t>(std::time(nullptr)); }

std::int64_t parse_cookie_date(std::string_view text) noexcept {
  // Tolerated formats: "Wdy, DD Mon YYYY HH:MM:SS GMT", "Wdy, DD-Mon-YYYY HH:MM:SS GMT".
  static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};
  std::string cleaned;
  for (const char c : text) {
    if (c == ',') {
      cleaned.push_back(' ');
    } else if (c == '-') {
      cleaned.push_back(' ');
    } else {
      cleaned.push_back(c);
    }
  }
  const std::vector<std::string_view> parts = split(cleaned, ' ');
  int day = -1;
  int month = -1;
  int year = -1;
  int hour = 0;
  int minute = 0;
  int second = 0;

  for (const std::string_view raw : parts) {
    const std::string_view part = trim(raw);
    if (part.empty()) continue;
    unsigned long long number = 0;
    if (parse_u64(part, number)) {
      if (day < 0) {
        day = static_cast<int>(number);
      } else if (year < 0 && part.size() >= 4) {
        year = static_cast<int>(number);
      } else {
        continue;
      }
      continue;
    }
    if (part.find(':') != std::string_view::npos) {
      std::vector<std::string_view> clock = split(part, ':');
      if (clock.size() == 3) {
        unsigned long long h = 0;
        unsigned long long m = 0;
        unsigned long long s = 0;
        if (parse_u64(trim(clock[0]), h) && parse_u64(trim(clock[1]), m) && parse_u64(trim(clock[2]), s)) {
          hour = static_cast<int>(h);
          minute = static_cast<int>(m);
          second = static_cast<int>(s);
        }
      }
      continue;
    }
    const std::string lowered = lower(part.substr(0, 3));
    for (int index = 0; index < 12; ++index) {
      if (lowered == months[index]) {
        month = index + 1;
        break;
      }
    }
  }
  if (day < 0 || month < 0 || year < 100) return 0;
  if (year < 100) year += 2000;

  std::tm utc{};
  utc.tm_year = year - 1900;
  utc.tm_mon = month - 1;
  utc.tm_mday = day;
  utc.tm_hour = hour;
  utc.tm_min = minute;
  utc.tm_sec = second;
#if defined(_WIN32)
  return static_cast<std::int64_t>(_mkgmtime(&utc));
#else
  return static_cast<std::int64_t>(timegm(&utc));
#endif
}

std::string default_cookie_path(const Url& url) {
  const std::string path = path_of(url);
  if (path.empty() || path.front() != '/') return "/";
  const std::size_t slash = path.rfind('/');
  if (slash == 0 || slash == std::string::npos) return "/";
  return path.substr(0, slash);
}

bool CookieJar::store_from_header(const Url& url, std::string_view header) {
  const std::vector<std::string_view> pieces = split(header, ';');
  if (pieces.empty()) return false;
  const std::string_view pair = trim(pieces.front());
  const std::size_t equals = pair.find('=');
  if (equals == std::string_view::npos) return false;
  Cookie cookie;
  cookie.name = std::string(trim(pair.substr(0, equals)));
  cookie.value = std::string(trim(pair.substr(equals + 1)));
  if (cookie.name.empty()) return false;

  const std::string host = host_of(url);
  cookie.domain = host;
  cookie.path = default_cookie_path(url);

  bool have_domain = false;
  bool have_path = false;
  bool delete_cookie = false;

  for (std::size_t index = 1; index < pieces.size(); ++index) {
    const std::string_view attribute = trim(pieces[index]);
    if (attribute.empty()) continue;
    const std::size_t split_at = attribute.find('=');
    const std::string key = lower(trim(attribute.substr(0, split_at)));
    const std::string_view value =
        split_at == std::string_view::npos ? std::string_view{} : trim(attribute.substr(split_at + 1));

    if (key == "domain") {
      std::string domain = lower(value);
      if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
      if (domain.empty()) continue;
      if (!domain_matches(host, domain)) return false;  // reject cookies for foreign domains
      if (is_ip_address(host) && domain != host) return false;
      cookie.domain = domain;
      cookie.host_only = false;
      have_domain = true;
    } else if (key == "path") {
      if (!value.empty() && value.front() == '/') {
        cookie.path = std::string(value);
        have_path = true;
      }
    } else if (key == "expires") {
      const std::int64_t when = parse_cookie_date(value);
      if (when != 0) cookie.expires_at = when;
      if (when != 0 && when <= current_unix_time()) delete_cookie = true;
    } else if (key == "max-age") {
      char* end = nullptr;
      const std::string text(value);
      const long long seconds = std::strtoll(text.c_str(), &end, 10);
      if (end != nullptr && *end == '\0') {
        if (seconds <= 0) {
          delete_cookie = true;
        } else {
          cookie.expires_at = current_unix_time() + seconds;
        }
      }
    } else if (key == "secure") {
      cookie.secure = true;
    } else if (key == "httponly") {
      cookie.http_only = true;
    }
  }
  (void)have_domain;
  (void)have_path;

  // Replace an existing cookie with the same (name, domain, path).
  for (auto it = cookies_.begin(); it != cookies_.end(); ++it) {
    if (it->name == cookie.name && it->domain == cookie.domain && it->path == cookie.path) {
      cookies_.erase(it);
      break;
    }
  }
  if (delete_cookie) return true;
  cookies_.push_back(std::move(cookie));
  return true;
}

void CookieJar::purge(std::int64_t now) {
  const std::int64_t stamp = now_or_current(now);
  cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                [stamp](const Cookie& cookie) { return cookie.expired(stamp); }),
                 cookies_.end());
}

std::string CookieJar::cookie_header(const Url& url, std::int64_t now) const {
  const std::int64_t stamp = now_or_current(now);
  const std::string host = host_of(url);
  const std::string path = path_of(url);
  const bool secure_page = url.scheme == "https";

  std::vector<const Cookie*> matching;
  for (const Cookie& cookie : cookies_) {
    if (cookie.expired(stamp)) continue;
    if (cookie.secure && !secure_page) continue;
    const bool domain_ok = cookie.host_only ? host == cookie.domain : domain_matches(host, cookie.domain);
    if (!domain_ok) continue;
    if (!path_matches(path, cookie.path)) continue;
    matching.push_back(&cookie);
  }
  // Longest path first (RFC 6265 §5.4 step 2).
  std::stable_sort(matching.begin(), matching.end(), [](const Cookie* a, const Cookie* b) {
    if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
    return a->name < b->name;
  });

  std::string out;
  for (const Cookie* cookie : matching) {
    if (!out.empty()) out += "; ";
    out += cookie->name;
    out += '=';
    out += cookie->value;
  }
  return out;
}

std::string CookieJar::document_cookie(const Url& url, std::int64_t now) const {
  const std::int64_t stamp = now_or_current(now);
  const std::string host = host_of(url);
  const std::string path = path_of(url);
  const bool secure_page = url.scheme == "https";

  std::vector<const Cookie*> matching;
  for (const Cookie& cookie : cookies_) {
    if (cookie.http_only || cookie.expired(stamp)) continue;
    if (cookie.secure && !secure_page) continue;
    const bool domain_ok = cookie.host_only ? host == cookie.domain : domain_matches(host, cookie.domain);
    if (!domain_ok) continue;
    if (!path_matches(path, cookie.path)) continue;
    matching.push_back(&cookie);
  }
  std::stable_sort(matching.begin(), matching.end(), [](const Cookie* a, const Cookie* b) {
    if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
    return a->name < b->name;
  });

  std::string out;
  for (const Cookie* cookie : matching) {
    if (!out.empty()) out += "; ";
    out += cookie->name;
    out += '=';
    out += cookie->value;
  }
  return out;
}

void CookieJar::set_document_cookie(const Url& url, std::string_view text) {
  // A page can only set non-HttpOnly cookies: strip the attribute if present.
  std::string header(text);
  const std::string lowered = lower(header);
  std::size_t httponly = lowered.find("httponly");
  while (httponly != std::string::npos) {
    header.erase(httponly, 8);
    httponly = lower(header).find("httponly");
  }
  (void)store_from_header(url, header);
}

}  // namespace bonk::net
