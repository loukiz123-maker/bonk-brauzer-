// Bonk Brauzer — named character references.
//
// The complete WHATWG table (2231 entries) is generated into src/html/entities.cpp;
// unknown names stay literal text (a bare '&' is legal HTML). Two-code-point
// references such as `&NotEqualTilde;` are supported.
#pragma once

#include <cstddef>
#include <string_view>

namespace bonk::html {

struct EntityEntry {
  std::string_view name;  // without '&' and without the terminating ';'
  char32_t code_point;
  char32_t second = 0;    // non-zero for two-code-point entities
  bool semicolon = false; // true when the reference is only valid with a trailing ';'
};

/// Longest entity matching the start of `input`; `consumed` includes the ';' when
/// the matched entry requires one. Returns nullptr when nothing matches.
[[nodiscard]] const EntityEntry* find_entity(std::string_view input, std::size_t& consumed);

}  // namespace bonk::html
