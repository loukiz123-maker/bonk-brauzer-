// Bonk Brauzer — selector engine (CSS Selectors subset) used by both
// `document.querySelector` and the CSS cascade.
//
// Supported: type, `*`, `.class`, `#id`, `[attr]`, `[attr="v"]`, `[attr~="v"]`,
// `[attr|="v"]`, `[attr^="v"]`, `[attr$="v"]`, `[attr*="v"]`, descendant and `>`
// combinators, selector lists (`a, b`), and the pseudo-classes `:first-child`,
// `:last-child`, `:only-child`, `:root`, `:not(<simple>)`, `:link`.
// Not yet: sibling combinators (`+`, `~`), `:nth-child()` with formulae.
//
// Specificity is (id, class/attr/pseudo-class, type) — computed once at parse
// time, so the cascade does not recompute it per element.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk::dom {

class Document;
struct NodeId;

struct SimpleSelector {
  std::string tag;                 // empty = any
  std::string id;
  std::vector<std::string> classes;
  struct AttributeTest {
    enum class Op { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };
    std::string name;
    Op op = Op::Exists;
    std::string value;
  };
  std::vector<AttributeTest> attributes;
  std::vector<std::string> pseudo_classes;  // lowercase, e.g. "first-child"
  std::vector<SimpleSelector> not_arguments;
};

struct ComplexSelector {
  std::vector<SimpleSelector> parts;
  // combinators[i] joins parts[i] and parts[i+1]; ' ' descendant, '>' child.
  std::vector<char> combinators;

  struct Specificity {
    int ids = 0;
    int classes = 0;
    int types = 0;
    [[nodiscard]] std::uint64_t packed() const noexcept {
      return (static_cast<std::uint64_t>(ids) << 40) | (static_cast<std::uint64_t>(classes) << 20) |
             static_cast<std::uint64_t>(types);
    }
    friend bool operator<(const Specificity& a, const Specificity& b) noexcept { return a.packed() < b.packed(); }
    friend bool operator==(const Specificity& a, const Specificity& b) noexcept {
      return a.packed() == b.packed();
    }
  };
  Specificity specificity;

  [[nodiscard]] std::string text() const;  // canonical form, handy for tests/logs
};

/// Parse a selector list ("h1, p.note > span"). Errors carry code "selector.*".
[[nodiscard]] Result<std::vector<ComplexSelector>> parse_selector_list(std::string_view input);

/// Match `element` against one complex selector (right-to-left, with ancestor walk).
[[nodiscard]] bool matches(const Document& document, NodeId element, const ComplexSelector& selector);
[[nodiscard]] bool matches_any(const Document& document, NodeId element,
                               const std::vector<ComplexSelector>& selectors);

}  // namespace bonk::dom
