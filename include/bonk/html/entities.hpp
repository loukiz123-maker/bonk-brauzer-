// Bonk Brauzer — named character references (subset).
//
// The full WHATWG table has ~2200 entries; we ship the ones that actually occur
// in hand-written and generated HTML plus the punctuation set. Unknown names are
// left as literal text (the tokenizer reports nothing: a bare '&' is legal).
#pragma once

#include <cstddef>
#include <string_view>

namespace bonk::html {

struct EntityEntry {
  std::string_view name;  // without '&' and ';'
  char32_t code_point;
  char32_t second = 0;  // non-zero for two-code-point entities
};

/// Longest entity name that starts at `input` (no ';' required), or nullptr.
[[nodiscard]] const EntityEntry* find_entity(std::string_view input, std::size_t& consumed);

}  // namespace bonk::html
