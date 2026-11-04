#include "bonk/dom/selector.hpp"

#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"
#include "bonk/dom/dom.hpp"

namespace bonk::dom {
namespace {

bool is_ident_char(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c >= 0x80;
}

Error selector_error(std::string_view message) { return Error::make("selector.invalid", message); }

std::string_view read_ident(std::string_view input, std::size_t& i) {
  const std::size_t start = i;
  while (i < input.size() && is_ident_char(static_cast<unsigned char>(input[i]))) ++i;
  return input.substr(start, i - start);
}

void skip_spaces(std::string_view input, std::size_t& i) {
  while (i < input.size() && is_ascii_whitespace(input[i])) ++i;
}

bool parse_attribute_selector(std::string_view input, std::size_t& i, SimpleSelector& simple, Error& error) {
  ++i;  // '['
  skip_spaces(input, i);
  const std::string_view name = read_ident(input, i);
  if (name.empty()) {
    error = selector_error("expected attribute name after '['");
    return false;
  }
  skip_spaces(input, i);

  SimpleSelector::AttributeTest test;
  test.name = to_ascii_lower(name);

  if (i < input.size() && input[i] == ']') {
    ++i;
    test.op = SimpleSelector::AttributeTest::Op::Exists;
    simple.attributes.push_back(std::move(test));
    return true;
  }

  auto consume = [&](std::string_view token) {
    if (input.compare(i, token.size(), token) != 0) return false;
    i += token.size();
    return true;
  };

  if (consume("=")) {
    test.op = SimpleSelector::AttributeTest::Op::Equals;
  } else if (consume("~=")) {
    test.op = SimpleSelector::AttributeTest::Op::Includes;
  } else if (consume("|=")) {
    test.op = SimpleSelector::AttributeTest::Op::DashMatch;
  } else if (consume("^=")) {
    test.op = SimpleSelector::AttributeTest::Op::Prefix;
  } else if (consume("$=")) {
    test.op = SimpleSelector::AttributeTest::Op::Suffix;
  } else if (consume("*=")) {
    test.op = SimpleSelector::AttributeTest::Op::Substring;
  } else {
    error = selector_error("expected one of =, ~=, |=, ^=, $=, *= in attribute selector");
    return false;
  }

  skip_spaces(input, i);
  if (i >= input.size() || (input[i] != '"' && input[i] != '\'')) {
    error = selector_error("expected a quoted attribute value");
    return false;
  }
  const char quote = input[i++];
  const std::size_t value_start = i;
  while (i < input.size() && input[i] != quote) ++i;
  if (i >= input.size()) {
    error = selector_error("unterminated attribute value");
    return false;
  }
  test.value = std::string(input.substr(value_start, i - value_start));
  ++i;  // closing quote
  skip_spaces(input, i);
  if (i >= input.size() || input[i] != ']') {
    error = selector_error("expected ']'");
    return false;
  }
  ++i;
  simple.attributes.push_back(std::move(test));
  return true;
}

// Selector specificity contribution of one simple selector.
void accumulate_specificity(const SimpleSelector& simple, ComplexSelector::Specificity& specificity) {
  if (!simple.id.empty()) specificity.ids += 1;
  specificity.classes += static_cast<int>(simple.classes.size());
  specificity.classes += static_cast<int>(simple.attributes.size());
  for (const std::string& pseudo : simple.pseudo_classes) {
    if (pseudo == "not") continue;  // :not() takes the specificity of its argument
    specificity.classes += 1;
  }
  if (!simple.tag.empty() && simple.tag != "*") specificity.types += 1;
  for (const SimpleSelector& inner : simple.not_arguments) accumulate_specificity(inner, specificity);
}

bool parse_compound(std::string_view input, std::size_t& i, SimpleSelector& simple, Error& error) {
  bool any = false;
  while (i < input.size()) {
    const char c = input[i];
    if (is_ascii_whitespace(c) || c == '>' || c == ',' || c == ')') break;

    if (c == '*') {
      simple.tag = "*";
      ++i;
      any = true;
      continue;
    }
    if (c == '.') {
      ++i;
      const std::string_view name = read_ident(input, i);
      if (name.empty()) {
        error = selector_error("expected class name after '.'");
        return false;
      }
      simple.classes.emplace_back(name);
      any = true;
      continue;
    }
    if (c == '#') {
      ++i;
      const std::string_view name = read_ident(input, i);
      if (name.empty()) {
        error = selector_error("expected id after '#'");
        return false;
      }
      if (!simple.id.empty()) {
        error = selector_error("an element cannot have two ids");
        return false;
      }
      simple.id = std::string(name);
      any = true;
      continue;
    }
    if (c == '[') {
      if (!parse_attribute_selector(input, i, simple, error)) return false;
      any = true;
      continue;
    }
    if (c == ':') {
      ++i;
      if (i < input.size() && input[i] == ':') {
        error = selector_error("pseudo-elements are not supported");
        return false;
      }
      const std::string_view name = read_ident(input, i);
      if (name.empty()) {
        error = selector_error("expected pseudo-class name after ':'");
        return false;
      }
      const std::string pseudo = to_ascii_lower(name);
      if (i < input.size() && input[i] == '(') {
        if (pseudo != "not") {
          error = selector_error("functional pseudo-class ':" + pseudo + "()' is not supported");
          return false;
        }
        ++i;
        skip_spaces(input, i);
        SimpleSelector inner;
        if (!parse_compound(input, i, inner, error)) return false;
        skip_spaces(input, i);
        if (i >= input.size() || input[i] != ')') {
          error = selector_error("expected ')' closing :not(");
          return false;
        }
        ++i;
        simple.pseudo_classes.push_back("not");
        simple.not_arguments.push_back(std::move(inner));
        any = true;
        continue;
      }
      simple.pseudo_classes.push_back(pseudo);
      any = true;
      continue;
    }
    if (is_ident_char(static_cast<unsigned char>(c))) {
      const std::string_view name = read_ident(input, i);
      if (!simple.tag.empty()) {
        error = selector_error("unexpected type selector '" + std::string(name) + "'");
        return false;
      }
      simple.tag = to_ascii_lower(name);
      any = true;
      continue;
    }
    error = selector_error("unexpected character '" + std::string(1, c) + "' in selector");
    return false;
  }
  if (!any) {
    error = selector_error("expected a selector");
    return false;
  }
  return true;
}

std::vector<std::string_view> split_selector_list(std::string_view input) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  int depth = 0;
  char quote = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '[' || c == '(') {
      ++depth;
    } else if (c == ']' || c == ')') {
      --depth;
    } else if (c == ',' && depth == 0) {
      parts.push_back(trim(input.substr(start, i - start)));
      start = i + 1;
    }
  }
  parts.push_back(trim(input.substr(start)));
  return parts;
}

bool matches_simple(const Document& document, NodeId element, const SimpleSelector& simple);

bool matches_attribute(const Document& document, NodeId element, const SimpleSelector::AttributeTest& test) {
  const std::string_view value = document.attribute(element, test.name);
  if (test.op == SimpleSelector::AttributeTest::Op::Exists) {
    const std::span<const Attribute> attributes = document.attributes(element);
    for (const Attribute& attribute : attributes) {
      if (attribute.name == test.name) return true;
    }
    return false;
  }
  switch (test.op) {
    case SimpleSelector::AttributeTest::Op::Exists:
      return true;
    case SimpleSelector::AttributeTest::Op::Equals:
      return value == test.value;
    case SimpleSelector::AttributeTest::Op::Includes: {
      std::size_t start = 0;
      while (start <= value.size()) {
        const std::size_t space = value.find(' ', start);
        const std::string_view token =
            value.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
        if (token == test.value) return true;
        if (space == std::string_view::npos) break;
        start = space + 1;
      }
      return false;
    }
    case SimpleSelector::AttributeTest::Op::DashMatch:
      return value == test.value ||
             (value.size() > test.value.size() && value.compare(0, test.value.size(), test.value) == 0 &&
              value[test.value.size()] == '-');
    case SimpleSelector::AttributeTest::Op::Prefix:
      return value.rfind(test.value, 0) == 0;
    case SimpleSelector::AttributeTest::Op::Suffix:
      return value.size() >= test.value.size() &&
             value.compare(value.size() - test.value.size(), test.value.size(), test.value) == 0;
    case SimpleSelector::AttributeTest::Op::Substring:
      return value.find(test.value) != std::string_view::npos;
  }
  return false;
}

bool matches_pseudo(const Document& document, NodeId element, const std::string& pseudo) {
  if (pseudo == "root") return element == document.document_element();
  if (pseudo == "first-child") {
    const NodeId parent = document.parent(element);
    if (!parent.valid()) return false;
    for (const NodeId kid : document.children(parent)) {
      if (document.type(kid) != NodeType::Element) continue;
      return kid == element;
    }
    return false;
  }
  if (pseudo == "last-child") {
    const NodeId parent = document.parent(element);
    if (!parent.valid()) return false;
    NodeId last;
    for (const NodeId kid : document.children(parent)) {
      if (document.type(kid) == NodeType::Element) last = kid;
    }
    return last == element;
  }
  if (pseudo == "only-child") {
    const NodeId parent = document.parent(element);
    if (!parent.valid()) return false;
    int count = 0;
    for (const NodeId kid : document.children(parent)) {
      if (document.type(kid) == NodeType::Element) ++count;
    }
    return count == 1;
  }
  if (pseudo == "link") {
    const std::string_view tag = document.tag_name(element);
    return (tag == "a" || tag == "area" || tag == "link") && document.attribute(element, "href").size() > 0;
  }
  if (pseudo == "hover" || pseudo == "focus" || pseudo == "active" || pseudo == "visited") return false;
  return false;
}

bool matches_simple(const Document& document, NodeId element, const SimpleSelector& simple) {
  if (document.type(element) != NodeType::Element) return false;
  if (!simple.tag.empty() && simple.tag != "*" && document.tag_name(element) != simple.tag) return false;
  if (!simple.id.empty() && document.attribute(element, "id") != simple.id) return false;
  for (const std::string& class_name : simple.classes) {
    if (!document.has_class(element, class_name)) return false;
  }
  for (const SimpleSelector::AttributeTest& test : simple.attributes) {
    if (!matches_attribute(document, element, test)) return false;
  }
  for (const std::string& pseudo : simple.pseudo_classes) {
    if (pseudo == "not") continue;
    if (!matches_pseudo(document, element, pseudo)) return false;
  }
  for (const SimpleSelector& inner : simple.not_arguments) {
    if (matches_simple(document, element, inner)) return false;
  }
  return true;
}

bool match_complex(const Document& document, NodeId element, const ComplexSelector& selector, std::size_t index) {
  if (!matches_simple(document, element, selector.parts[index])) return false;
  if (index == 0) return true;

  const char combinator = selector.combinators[index - 1];
  NodeId ancestor = document.parent(element);
  if (combinator == '>') {
    return ancestor.valid() && match_complex(document, ancestor, selector, index - 1);
  }
  while (ancestor.valid()) {
    if (match_complex(document, ancestor, selector, index - 1)) return true;
    ancestor = document.parent(ancestor);
  }
  return false;
}

}  // namespace

std::string ComplexSelector::text() const {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += combinators[i - 1] == '>' ? " > " : " ";
    }
    const SimpleSelector& simple = parts[i];
    if (!simple.tag.empty()) {
      out += simple.tag;
    } else if (simple.id.empty() && simple.classes.empty() && simple.attributes.empty() &&
               simple.pseudo_classes.empty()) {
      out += '*';
    }
    if (!simple.id.empty()) {
      out += '#';
      out += simple.id;
    }
    for (const std::string& class_name : simple.classes) {
      out += '.';
      out += class_name;
    }
    for (const SimpleSelector::AttributeTest& test : simple.attributes) {
      out += '[';
      out += test.name;
      out += ']';
    }
    for (const std::string& pseudo : simple.pseudo_classes) {
      out += ':';
      out += pseudo;
    }
  }
  return out;
}

Result<std::vector<ComplexSelector>> parse_selector_list(std::string_view input) {
  std::vector<ComplexSelector> out;
  for (const std::string_view part : split_selector_list(input)) {
    if (part.empty()) {
      return Result<std::vector<ComplexSelector>>::failure(selector_error("empty selector in the list"));
    }
    ComplexSelector complex;
    std::size_t i = 0;
    skip_spaces(part, i);
    Error error;
    if (!parse_compound(part, i, complex.parts.emplace_back(), error)) {
      return Result<std::vector<ComplexSelector>>::failure(error);
    }
    while (true) {
      bool had_space = false;
      while (i < part.size() && is_ascii_whitespace(part[i])) {
        ++i;
        had_space = true;
      }
      if (i >= part.size()) break;

      char combinator = ' ';
      if (part[i] == '>') {
        combinator = '>';
        ++i;
        skip_spaces(part, i);
      } else if (!had_space) {
        return Result<std::vector<ComplexSelector>>::failure(selector_error("expected a combinator"));
      }
      complex.combinators.push_back(combinator);
      SimpleSelector next;
      if (!parse_compound(part, i, next, error)) {
        return Result<std::vector<ComplexSelector>>::failure(error);
      }
      complex.parts.push_back(std::move(next));
    }

    for (const SimpleSelector& simple : complex.parts) accumulate_specificity(simple, complex.specificity);
    out.push_back(std::move(complex));
  }
  return Result<std::vector<ComplexSelector>>::success(std::move(out));
}

bool matches(const Document& document, NodeId element, const ComplexSelector& selector) {
  if (selector.parts.empty()) return false;
  return match_complex(document, element, selector, selector.parts.size() - 1);
}

bool matches_any(const Document& document, NodeId element, const std::vector<ComplexSelector>& selectors) {
  for (const ComplexSelector& selector : selectors) {
    if (matches(document, element, selector)) return true;
  }
  return false;
}

}  // namespace bonk::dom
