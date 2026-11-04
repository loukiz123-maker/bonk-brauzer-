#include "json.hpp"

#include <cstdlib>
#include <string>

namespace bonk::conformance::json {
namespace {

struct Reader {
  std::string_view text;
  std::size_t position = 0;

  void skip_space() {
    while (position < text.size()) {
      const char c = text[position];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] bool at_end() const noexcept { return position >= text.size(); }

  static void append_utf8(std::string& out, unsigned code_point) {
    if (code_point < 0x80) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  [[nodiscard]] bool hex4(unsigned& out) {
    if (position + 4 > text.size()) return false;
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text[position + static_cast<std::size_t>(i)];
      unsigned digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<unsigned>(c - 'A' + 10);
      } else {
        return false;
      }
      value = value * 16 + digit;
    }
    position += 4;
    out = value;
    return true;
  }

  [[nodiscard]] bool parse_string(std::string& out) {
    if (at_end() || text[position] != '"') return false;
    ++position;
    out.clear();
    while (position < text.size()) {
      const char c = text[position++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (at_end()) return false;
      const char escape = text[position++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned first = 0;
          if (!hex4(first)) return false;
          unsigned code_point = first;
          if (first >= 0xD800 && first <= 0xDBFF && position + 1 < text.size() && text[position] == '\\' &&
              text[position + 1] == 'u') {
            position += 2;
            unsigned second = 0;
            if (!hex4(second)) return false;
            if (second >= 0xDC00 && second <= 0xDFFF) {
              code_point = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
            }
          }
          append_utf8(out, code_point);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  [[nodiscard]] bool parse_value(Value& out) {
    skip_space();
    if (at_end()) return false;
    const char c = text[position];
    if (c == '{') {
      ++position;
      out.kind = Value::Kind::Object;
      skip_space();
      if (!at_end() && text[position] == '}') {
        ++position;
        return true;
      }
      for (;;) {
        skip_space();
        std::string key;
        if (!parse_string(key)) return false;
        skip_space();
        if (at_end() || text[position] != ':') return false;
        ++position;
        Value value;
        if (!parse_value(value)) return false;
        out.members.emplace_back(std::move(key), std::move(value));
        skip_space();
        if (at_end()) return false;
        if (text[position] == ',') {
          ++position;
          continue;
        }
        if (text[position] == '}') {
          ++position;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++position;
      out.kind = Value::Kind::Array;
      skip_space();
      if (!at_end() && text[position] == ']') {
        ++position;
        return true;
      }
      for (;;) {
        Value item;
        if (!parse_value(item)) return false;
        out.items.push_back(std::move(item));
        skip_space();
        if (at_end()) return false;
        if (text[position] == ',') {
          ++position;
          continue;
        }
        if (text[position] == ']') {
          ++position;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out.kind = Value::Kind::String;
      return parse_string(out.text);
    }
    if (text.compare(position, 4, "true") == 0) {
      out.kind = Value::Kind::Bool;
      out.boolean = true;
      position += 4;
      return true;
    }
    if (text.compare(position, 5, "false") == 0) {
      out.kind = Value::Kind::Bool;
      out.boolean = false;
      position += 5;
      return true;
    }
    if (text.compare(position, 4, "null") == 0) {
      out.kind = Value::Kind::Null;
      position += 4;
      return true;
    }
    // number
    const char* begin = text.data() + position;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) return false;
    position += static_cast<std::size_t>(end - begin);
    out.kind = Value::Kind::Number;
    out.number = value;
    return true;
  }
};

}  // namespace

bool parse(std::string_view text, Value& out) {
  Reader reader{text, 0};
  out = Value{};
  if (!reader.parse_value(out)) {
    out = Value{};
    return false;
  }
  return true;
}

std::string unescape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      out.push_back(text[i]);
      continue;
    }
    const char escape = text[++i];
    switch (escape) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        if (i + 4 >= text.size()) {
          out.push_back('\\');
          out.push_back(escape);
          break;
        }
        unsigned code_point = 0;
        bool ok = true;
        for (int digit_index = 0; digit_index < 4; ++digit_index) {
          const char c = text[i + 1 + static_cast<std::size_t>(digit_index)];
          unsigned digit = 0;
          if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
          } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
          } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
          } else {
            ok = false;
            break;
          }
          code_point = code_point * 16 + digit;
        }
        if (!ok) {
          out.push_back('\\');
          out.push_back('u');
          break;
        }
        i += 4;
        if (code_point < 0x80) {
          out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
          out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
          out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        break;
      }
      default:
        out.push_back('\\');
        out.push_back(escape);
        break;
    }
  }
  return out;
}

}  // namespace bonk::conformance::json
