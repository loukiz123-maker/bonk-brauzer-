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

/// "https://cdn.example.com:8443/js" -> host/port/path parts, no allocation-heavy URL parse.
struct HostSource {
  std::string scheme;      // empty = any
  std::string host;        // lowercase, may start with "*."
  std::uint16_t port = 0;  // 0 = any
  std::string path_prefix;
};

bool parse_host_source(std::string_view text, HostSource& out) {
  std::string_view rest = text;
  const std::size_t scheme_end = rest.find("://");
  if (scheme_end != std::string_view::npos) {
    out.scheme = lower(rest.substr(0, scheme_end));
    rest = rest.substr(scheme_end + 3);
  }
  const std::size_t slash = rest.find('/');
  std::string_view authority = rest;
  if (slash != std::string_view::npos) {
    authority = rest.substr(0, slash);
    out.path_prefix = std::string(rest.substr(slash));
  }
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
    const std::string_view port = authority.substr(colon + 1);
    unsigned long long number = 0;
    if (parse_u64(port, number) && number <= 65535) {
      out.port = static_cast<std::uint16_t>(number);
      authority = authority.substr(0, colon);
    }
  }
  out.host = lower(authority);
  return !out.host.empty();
}

bool host_matches(const HostSource& source, const net::Url& url) {
  if (!source.scheme.empty() && source.scheme != url.scheme) return false;
  if (source.port != 0 && source.port != url.effective_port()) return false;
  if (!source.path_prefix.empty() && !url.path.starts_with(source.path_prefix)) return false;
  const std::string host = lower(url.host);
  if (source.host == "*") return true;
  if (source.host.starts_with("*.")) return host.ends_with(source.host.substr(1));
  return host == source.host;
}

bool is_keyword(std::string_view source, std::string_view keyword) {
  return source.size() >= 2 && source.front() == '\'' && source.back() == '\'' &&
         lower(source.substr(1, source.size() - 2)) == keyword;
}

}  // namespace

CspPolicy CspPolicy::parse(std::string_view header) {
  CspPolicy policy;
  for (const std::string_view raw_directive : split(header, ';')) {
    const std::string_view directive = trim(raw_directive);
    if (directive.empty()) continue;
    // Directive name, then whitespace-separated sources.
    const std::size_t space = directive.find_first_of(" \t");
    const std::string name = lower(space == std::string_view::npos ? directive : directive.substr(0, space));
    std::vector<std::string> sources;
    if (space != std::string_view::npos) {
      std::string_view rest = directive.substr(space);
      while (!rest.empty()) {
        rest = trim(rest);
        if (rest.empty()) break;
        const std::size_t end = rest.find_first_of(" \t");
        sources.push_back(lower(end == std::string_view::npos ? rest : rest.substr(0, end)));
        if (end == std::string_view::npos) break;
        rest = rest.substr(end);
      }
    }
    if (name.empty()) continue;

    Directive* existing = nullptr;
    for (auto& [key, value] : policy.directives_) {
      if (key == name) existing = &value;
    }
    if (existing == nullptr) {
      policy.directives_.emplace_back(name, Directive{true, std::move(sources)});
    } else {
      existing->present = true;
      existing->sources.insert(existing->sources.end(), sources.begin(), sources.end());
    }
  }
  return policy;
}

const CspPolicy::Directive* CspPolicy::find(std::string_view name) const {
  for (const auto& [key, value] : directives_) {
    if (key == name) return value.present ? &value : nullptr;
  }
  return nullptr;
}

bool CspPolicy::allows(std::string_view directive, std::string_view url_text, const Origin& self,
                       bool inline_allowed) const {
  if (directives_.empty()) return true;
  // `default-src` is the fallback for every fetch directive.
  const Directive* rule = find(directive);
  if (rule == nullptr) rule = find("default-src");
  if (rule == nullptr) return true;  // directive absent and no default-src: unrestricted

  for (const std::string& source : rule->sources) {
    if (source == "'none'") continue;
    if (source == "*") return true;
    if (is_keyword(source, "unsafe-inline")) {
      if (url_text.empty() && inline_allowed) return true;
      continue;
    }
    if (is_keyword(source, "self")) {
      if (url_text.empty()) continue;
      const Result<net::Url> url = net::parse_url(url_text);
      if (url.is_error()) continue;
      if (Origin::from_url(url.value()) == self && !self.opaque) return true;
      continue;
    }
    if (!url_text.empty() && source.find("://") == std::string::npos && !source.empty() &&
        source.back() == ':' && source.find('.') == std::string::npos) {
      // Scheme source such as `https:`.
      const Result<net::Url> url = net::parse_url(url_text);
      if (url.is_ok() && url.value().scheme == source.substr(0, source.size() - 1)) return true;
      continue;
    }
    HostSource host_source;
    if (!parse_host_source(source, host_source)) continue;
    const Result<net::Url> url = net::parse_url(url_text);
    if (url.is_error()) continue;
    if (host_matches(host_source, url.value())) return true;
  }
  return false;
}

bool CspPolicy::allows_inline_script() const {
  if (directives_.empty()) return true;
  const Directive* rule = find("script-src");
  if (rule == nullptr) rule = find("default-src");
  if (rule == nullptr) return true;
  for (const std::string& source : rule->sources) {
    if (is_keyword(source, "unsafe-inline")) return true;
  }
  return false;
}

bool CspPolicy::allows_script(std::string_view url, const Origin& self) const {
  return allows("script-src", url, self, false);
}

bool CspPolicy::allows_style(std::string_view url, const Origin& self) const {
  return allows("style-src", url, self, true);
}

bool CspPolicy::allows_image(std::string_view url, const Origin& self) const {
  return allows("img-src", url, self, false);
}

bool CspPolicy::allows_connect(std::string_view url, const Origin& self) const {
  return allows("connect-src", url, self, false);
}

std::string CspPolicy::describe() const {
  if (directives_.empty()) return "none";
  std::string out;
  for (const auto& [name, directive] : directives_) {
    if (!out.empty()) out += "; ";
    out += name;
    for (const std::string& source : directive.sources) {
      out += ' ';
      out += source;
    }
  }
  return out;
}

}  // namespace bonk::security
