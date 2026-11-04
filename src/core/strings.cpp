#include "bonk/core/strings.hpp"

#include <cstdint>
#include <limits>

namespace bonk {
namespace {

inline bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
inline char upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

}  // namespace

std::string_view trim(std::string_view input) noexcept { return trim_right(trim_left(input)); }

std::string_view trim_left(std::string_view input) noexcept {
  std::size_t start = 0;
  while (start < input.size() && is_space(input[start])) ++start;
  return input.substr(start);
}

std::string_view trim_right(std::string_view input) noexcept {
  std::size_t end = input.size();
  while (end > 0 && is_space(input[end - 1])) --end;
  return input.substr(0, end);
}

std::vector<std::string_view> split(std::string_view input, char delimiter) {
  std::vector<std::string_view> parts;
  if (input.empty()) return parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = input.find(delimiter, start);
    if (position == std::string_view::npos) {
      parts.push_back(input.substr(start));
      return parts;
    }
    parts.push_back(input.substr(start, position - start));
    start = position + 1;
    if (start == input.size()) {  // trailing delimiter produces a trailing empty field
      parts.emplace_back();
      return parts;
    }
  }
}

std::string join(const std::vector<std::string_view>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.append(separator);
    out.append(parts[i]);
  }
  return out;
}

bool starts_with_ci(std::string_view haystack, std::string_view prefix) noexcept {
  if (prefix.size() > haystack.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (lower(haystack[i]) != lower(prefix[i])) return false;
  }
  return true;
}

bool equals_ci(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() && starts_with_ci(a, b);
}

std::string to_ascii_lower(std::string_view input) {
  std::string out(input);
  for (char& c : out) c = lower(c);
  return out;
}

std::string to_ascii_upper(std::string_view input) {
  std::string out(input);
  for (char& c : out) c = upper(c);
  return out;
}

std::string replace_all(std::string_view input, std::string_view needle, std::string_view replacement) {
  if (needle.empty()) return std::string(input);
  std::string out;
  out.reserve(input.size());
  std::size_t start = 0;
  while (true) {
    const std::size_t position = input.find(needle, start);
    if (position == std::string_view::npos) {
      out.append(input.substr(start));
      return out;
    }
    out.append(input.substr(start, position - start));
    out.append(replacement);
    start = position + needle.size();
  }
}

std::string pad_right(std::string_view input, std::size_t width) {
  std::string out(input);
  if (out.size() < width) out.append(width - out.size(), ' ');
  return out;
}

std::string pad_left(std::string_view input, std::size_t width) {
  std::string out;
  if (input.size() < width) out.append(width - input.size(), ' ');
  out.append(input);
  return out;
}

bool parse_u64(std::string_view input, unsigned long long& out) noexcept {
  if (input.empty()) return false;
  unsigned long long value = 0;
  for (const char c : input) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<unsigned long long>(c - '0');
    if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

}  // namespace bonk
