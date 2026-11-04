// Tree-walking interpreter for the Bonk Brauzer JS engine.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bonk/js/js.hpp"
#include "internal.hpp"
#include "ast.hpp"

namespace bonk::js {
namespace {

class Interpreter;

/// The interpreter currently running (single-threaded engine, one context at a time).
thread_local Interpreter* g_interpreter = nullptr;

// ------------------------------------------------------------------ scope ----

/// A lexical scope. Scopes are allocated in the runtime's arena and never freed
/// while the context lives: a closure may reference any enclosing scope, so a
/// refcounted parent pointer would create a cycle on every function declaration.
struct Scope {
  std::unordered_map<std::string, Value> variables;
  Scope* parent = nullptr;
  Value this_value;
  bool is_function_scope = false;
};

using ScopePtr = Scope*;

struct Lookup {
  bool found = false;
  ScopePtr scope;
};

Lookup lookup_scope(const ScopePtr& start, const std::string& name) {
  ScopePtr scope = start;
  while (scope != nullptr) {
    if (scope->variables.find(name) != scope->variables.end()) return Lookup{true, scope};
    scope = scope->parent;
  }
  return Lookup{};
}

// ------------------------------------------------------------- completion ----

enum class CompletionType { Normal, Return, Throw, Break, Continue };

struct Completion {
  CompletionType type = CompletionType::Normal;
  Value value;
};

Completion normal() { return Completion{}; }
Completion thrown(Value value) {
  Completion completion;
  completion.type = CompletionType::Throw;
  completion.value = std::move(value);
  return completion;
}

std::shared_ptr<Object> make_error_object(const std::string& name, const std::string& message) {
  auto error = std::make_shared<Object>();
  error->kind = Object::Kind::Error;
  error->properties["name"] = Property{Value::string_of(name)};
  error->properties["message"] = Property{Value::string_of(message)};
  return error;
}

Value make_error(const std::string& name, const std::string& message) {
  return Value::object_of(make_error_object(name, message));
}

Value type_error(const std::string& message) { return make_error("TypeError", message); }
Value reference_error(const std::string& message) { return make_error("ReferenceError", message); }

std::shared_ptr<Object> make_array(std::vector<Value> elements) {
  auto array = std::make_shared<Object>();
  array->kind = Object::Kind::Array;
  array->elements = std::move(elements);
  return array;
}

std::shared_ptr<Function> make_native(std::string name, Function::Native native) {
  auto function = std::make_shared<Function>();
  function->name = std::move(name);
  function->native = std::move(native);
  return function;
}

Value native_value(std::string name, Function::Native native) {
  return Value::function_of(make_native(std::move(name), std::move(native)));
}

// --------------------------------------------------------------- helpers ----

std::string number_to_raw_string(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  if (value == 0.0) return "0";
  if (value == std::floor(value) && std::fabs(value) < 1e21) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return buffer;
  }
  for (int precision = 1; precision <= 17; ++precision) {
    char candidate[40];
    std::snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
    if (std::strtod(candidate, nullptr) == value) return candidate;
  }
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return buffer;
}

std::string join_values(const std::vector<Value>& values, const std::string& separator) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += separator;
    out += to_string(values[i]);
  }
  return out;
}

std::string json_quote(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  out += '"';
  return out;
}

std::string json_serialize(const Value& value, int depth) {
  if (depth > 32) return "null";
  switch (value.type) {
    case ValueType::Undefined:
    case ValueType::Null:
      return "null";
    case ValueType::Boolean:
      return value.boolean ? "true" : "false";
    case ValueType::Number:
      return (std::isnan(value.number) || std::isinf(value.number)) ? "null" : number_to_raw_string(value.number);
    case ValueType::String:
      return json_quote(value.text);
    case ValueType::Function:
      return "null";
    case ValueType::Object: {
      if (value.object->is_array()) {
        std::string out = "[";
        for (std::size_t i = 0; i < value.object->elements.size(); ++i) {
          if (i != 0) out += ',';
          out += json_serialize(value.object->elements[i], depth + 1);
        }
        return out + "]";
      }
      std::vector<std::string> keys;
      for (const auto& entry : value.object->properties) keys.push_back(entry.first);
      std::sort(keys.begin(), keys.end());
      std::string out = "{";
      bool first = true;
      for (const std::string& key : keys) {
        const Property& property = value.object->properties.at(key);
        if (property.value.is_undefined() || property.value.type == ValueType::Function) continue;
        if (!first) out += ',';
        first = false;
        out += json_quote(key);
        out += ':';
        out += json_serialize(property.value, depth + 1);
      }
      return out + "}";
    }
  }
  return "null";
}

/// Minimal JSON parser (objects, arrays, strings, numbers, booleans, null).
struct JsonReader {
  std::string_view text;
  std::size_t position = 0;

  void skip() {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\n' || text[position] == '\t' ||
                                      text[position] == '\r')) {
      ++position;
    }
  }

  bool consume(char expected) {
    skip();
    if (position < text.size() && text[position] == expected) {
      ++position;
      return true;
    }
    return false;
  }

  Value parse() {
    skip();
    if (position >= text.size()) return Value::undefined();
    const char c = text[position];
    if (c == '{') {
      ++position;
      auto object = std::make_shared<Object>();
      skip();
      if (consume('}')) return Value::object_of(object);
      for (;;) {
        skip();
        if (position >= text.size() || text[position] != '"') return Value::undefined();
        const Value key = parse_string();
        if (!consume(':')) return Value::undefined();
        object->properties[key.text] = Property{parse()};
        if (consume(',')) continue;
        if (consume('}')) break;
        return Value::undefined();
      }
      return Value::object_of(object);
    }
    if (c == '[') {
      ++position;
      std::vector<Value> elements;
      skip();
      if (consume(']')) return Value::object_of(make_array(std::move(elements)));
      for (;;) {
        elements.push_back(parse());
        if (consume(',')) continue;
        if (consume(']')) break;
        return Value::undefined();
      }
      return Value::object_of(make_array(std::move(elements)));
    }
    if (c == '"') return parse_string();
    if (text.compare(position, 4, "true") == 0) {
      position += 4;
      return Value::boolean_of(true);
    }
    if (text.compare(position, 5, "false") == 0) {
      position += 5;
      return Value::boolean_of(false);
    }
    if (text.compare(position, 4, "null") == 0) {
      position += 4;
      return Value::null();
    }
    const char* start = text.data() + position;
    char* end = nullptr;
    const double number = std::strtod(start, &end);
    if (end == start) return Value::undefined();
    position += static_cast<std::size_t>(end - start);
    return Value::number_of(number);
  }

  Value parse_string() {
    ++position;  // opening quote
    std::string out;
    while (position < text.size() && text[position] != '"') {
      char c = text[position++];
      if (c == '\\' && position < text.size()) {
        const char escape = text[position++];
        switch (escape) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'u': {
            if (position + 4 <= text.size()) {
              const auto code_point =
                  static_cast<std::uint32_t>(std::strtoul(std::string(text.substr(position, 4)).c_str(), nullptr, 16));
              position += 4;
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
            }
            break;
          }
          default: out.push_back(escape); break;
        }
        continue;
      }
      out.push_back(c);
    }
    ++position;  // closing quote
    return Value::string_of(out);
  }
};

// -------------------------------------------------------------- runtime -----

struct RuntimeState {
  ScopePtr global = nullptr;
  std::vector<std::unique_ptr<Scope>> scope_arena;

  Scope* new_scope(Scope* parent) {
    scope_arena.push_back(std::make_unique<Scope>());
    Scope* scope = scope_arena.back().get();
    scope->parent = parent;
    return scope;
  }

  std::vector<ConsoleEntry> console;
  std::unordered_map<std::string, std::function<Value(const std::vector<Value>&)>> host_callbacks;
  std::vector<Value> timer_queue;
  std::string origin;

  void log(ConsoleEntry::Level level, const std::string& message) {
    console.push_back(ConsoleEntry{level, message});
    if (console.size() > 500) console.erase(console.begin());
  }
};

bool is_array_index(const std::string& key, std::size_t& index) {
  if (key.empty() || key.size() > 9) return false;
  std::size_t value = 0;
  for (const char c : key) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  index = value;
  return true;
}

// ------------------------------------------------------------ interpreter ----

class Interpreter {
 public:
  Interpreter(RuntimeState& runtime, ScopePtr global) : runtime_(runtime), global_(global) {}

  Value eval(const ast::ExprPtr& expression, const ScopePtr& scope);
  Completion exec(const ast::StmtPtr& statement, const ScopePtr& scope);
  Completion exec_statements(const std::vector<ast::StmtPtr>& statements, const ScopePtr& scope);
  Value call(const Value& callee, const Value& this_value, std::vector<Value> arguments);
  Value get_member(const Value& object, const std::string& key);
  void set_member(const Value& object, const std::string& key, const Value& value);
  RuntimeState& runtime() { return runtime_; }

 private:
  Value make_function(const ast::FunctionPtr& node, const ScopePtr& scope);
  Value eval_call(const ast::Expr& expression, const ScopePtr& scope);
  Completion exec_block(const std::vector<ast::StmtPtr>& statements, const ScopePtr& parent);

  RuntimeState& runtime_;
  ScopePtr global_;
};

Value call_function(const Value& callee, const Value& this_value, std::vector<Value> arguments);

// ---------------------------------------------------------- member access ----

Value Interpreter::get_member(const Value& object, const std::string& key) {
  if (object.is_nullish()) {
    throw thrown(type_error("cannot read property '" + key + "' of " + to_string(object)));
  }

  if (object.type == ValueType::Function && object.function != nullptr) {
    const std::shared_ptr<Function> function = object.function;
    if (key == "call") {
      return native_value("call", [function](const std::vector<Value>& arguments, Value) {
        const Value this_value = arguments.empty() ? Value::undefined() : arguments[0];
        std::vector<Value> rest(arguments.begin() + (arguments.empty() ? 0 : 1), arguments.end());
        return call_function(Value::function_of(function), this_value, std::move(rest));
      });
    }
    if (key == "apply") {
      return native_value("apply", [function](const std::vector<Value>& arguments, Value) {
        const Value this_value = arguments.empty() ? Value::undefined() : arguments[0];
        std::vector<Value> rest;
        if (arguments.size() > 1 && arguments[1].is_object() && arguments[1].object->is_array()) {
          rest = arguments[1].object->elements;
        }
        return call_function(Value::function_of(function), this_value, std::move(rest));
      });
    }
    if (key == "bind") {
      return native_value("bind", [function](const std::vector<Value>& arguments, Value) {
        const Value this_value = arguments.empty() ? Value::undefined() : arguments[0];
        std::vector<Value> bound(arguments.begin() + (arguments.empty() ? 0 : 1), arguments.end());
        return native_value("bound", [function, this_value, bound](const std::vector<Value>& extra, Value) {
          std::vector<Value> all = bound;
          all.insert(all.end(), extra.begin(), extra.end());
          return call_function(Value::function_of(function), this_value, std::move(all));
        });
      });
    }
    if (key == "name") return Value::string_of(function->name);
    if (key == "length") return Value::number_of(static_cast<double>(function->parameter_count));
    if (key == "prototype") {
      if (!function->prototype_object) function->prototype_object = std::make_shared<Object>();
      return Value::object_of(function->prototype_object);
    }
    return Value::undefined();
  }

  if (object.is_string()) {
    const std::string text = object.text;
    if (key == "length") return Value::number_of(static_cast<double>(text.size()));
    if (key == "toUpperCase" || key == "toLowerCase") {
      return native_value(key, [text, key](const std::vector<Value>&, Value) {
        std::string out = text;
        for (char& c : out) {
          if (key == "toUpperCase" && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
          if (key == "toLowerCase" && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return Value::string_of(out);
      });
    }
    if (key == "trim") {
      return native_value("trim", [text](const std::vector<Value>&, Value) {
        const std::size_t begin = text.find_first_not_of(" \t\n\r");
        if (begin == std::string::npos) return Value::string_of(std::string());
        const std::size_t end = text.find_last_not_of(" \t\n\r");
        return Value::string_of(text.substr(begin, end - begin + 1));
      });
    }
    if (key == "indexOf") {
      return native_value("indexOf", [text](const std::vector<Value>& arguments, Value) {
        const std::string needle = arguments.empty() ? std::string() : to_string(arguments[0]);
        const std::size_t found = text.find(needle);
        return Value::number_of(found == std::string::npos ? -1.0 : static_cast<double>(found));
      });
    }
    if (key == "includes") {
      return native_value("includes", [text](const std::vector<Value>& arguments, Value) {
        const std::string needle = arguments.empty() ? std::string() : to_string(arguments[0]);
        return Value::boolean_of(text.find(needle) != std::string::npos);
      });
    }
    if (key == "startsWith") {
      return native_value("startsWith", [text](const std::vector<Value>& arguments, Value) {
        const std::string needle = arguments.empty() ? std::string() : to_string(arguments[0]);
        return Value::boolean_of(text.rfind(needle, 0) == 0);
      });
    }
    if (key == "endsWith") {
      return native_value("endsWith", [text](const std::vector<Value>& arguments, Value) {
        const std::string needle = arguments.empty() ? std::string() : to_string(arguments[0]);
        return Value::boolean_of(text.size() >= needle.size() &&
                                 text.compare(text.size() - needle.size(), needle.size(), needle) == 0);
      });
    }
    if (key == "slice" || key == "substring") {
      return native_value(key, [text](const std::vector<Value>& arguments, Value) {
        const auto length = static_cast<long>(text.size());
        long begin = arguments.empty() ? 0 : static_cast<long>(to_number(arguments[0]));
        long end = arguments.size() < 2 ? length : static_cast<long>(to_number(arguments[1]));
        if (begin < 0) begin = std::max(0L, length + begin);
        if (end < 0) end = std::max(0L, length + end);
        begin = std::min(begin, length);
        end = std::min(end, length);
        if (end < begin) std::swap(begin, end);
        return Value::string_of(text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin)));
      });
    }
    if (key == "charAt") {
      return native_value("charAt", [text](const std::vector<Value>& arguments, Value) {
        const auto index = arguments.empty() ? 0 : static_cast<std::size_t>(to_number(arguments[0]));
        if (index >= text.size()) return Value::string_of(std::string());
        return Value::string_of(std::string(1, text[index]));
      });
    }
    if (key == "split") {
      return native_value("split", [text](const std::vector<Value>& arguments, Value) {
        std::vector<Value> parts;
        if (arguments.empty() || arguments[0].is_undefined()) {
          parts.push_back(Value::string_of(text));
        } else {
          const std::string separator = to_string(arguments[0]);
          if (separator.empty()) {
            for (const char c : text) parts.push_back(Value::string_of(std::string(1, c)));
          } else {
            std::size_t start = 0;
            for (;;) {
              const std::size_t found = text.find(separator, start);
              if (found == std::string::npos) {
                parts.push_back(Value::string_of(text.substr(start)));
                break;
              }
              parts.push_back(Value::string_of(text.substr(start, found - start)));
              start = found + separator.size();
            }
          }
        }
        return Value::object_of(make_array(std::move(parts)));
      });
    }
    if (key == "replace") {
      return native_value("replace", [text](const std::vector<Value>& arguments, Value) {
        if (arguments.size() < 2) return Value::string_of(text);
        const std::string needle = to_string(arguments[0]);
        const std::string replacement = to_string(arguments[1]);
        const std::size_t found = text.find(needle);
        if (found == std::string::npos) return Value::string_of(text);
        return Value::string_of(text.substr(0, found) + replacement + text.substr(found + needle.size()));
      });
    }
    if (key == "toString" || key == "valueOf") {
      return native_value(key, [text](const std::vector<Value>&, Value) { return Value::string_of(text); });
    }
    std::size_t index = 0;
    if (is_array_index(key, index)) {
      if (index < text.size()) return Value::string_of(std::string(1, text[index]));
    }
    return Value::undefined();
  }

  if (object.is_number()) {
    const double number = object.number;
    if (key == "toFixed") {
      return native_value("toFixed", [number](const std::vector<Value>& arguments, Value) {
        const int digits = arguments.empty() ? 0 : static_cast<int>(to_number(arguments[0]));
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(0, std::min(20, digits)), number);
        return Value::string_of(buffer);
      });
    }
    if (key == "toString" || key == "valueOf") {
      return native_value(key, [number](const std::vector<Value>&, Value) {
        return Value::string_of(number_to_raw_string(number));
      });
    }
    return Value::undefined();
  }

  if (!object.is_object() || object.object == nullptr) return Value::undefined();
  const std::shared_ptr<Object> target = object.object;

  if (target->is_array()) {
    if (key == "length") return Value::number_of(static_cast<double>(target->elements.size()));
    if (key == "push") {
      return native_value("push", [target](const std::vector<Value>& arguments, Value) {
        for (const Value& argument : arguments) target->elements.push_back(argument);
        return Value::number_of(static_cast<double>(target->elements.size()));
      });
    }
    if (key == "pop") {
      return native_value("pop", [target](const std::vector<Value>&, Value) {
        if (target->elements.empty()) return Value::undefined();
        const Value last = target->elements.back();
        target->elements.pop_back();
        return last;
      });
    }
    if (key == "shift") {
      return native_value("shift", [target](const std::vector<Value>&, Value) {
        if (target->elements.empty()) return Value::undefined();
        const Value first = target->elements.front();
        target->elements.erase(target->elements.begin());
        return first;
      });
    }
    if (key == "indexOf") {
      return native_value("indexOf", [target](const std::vector<Value>& arguments, Value) {
        if (arguments.empty()) return Value::number_of(-1);
        for (std::size_t i = 0; i < target->elements.size(); ++i) {
          if (strict_equals(target->elements[i], arguments[0])) return Value::number_of(static_cast<double>(i));
        }
        return Value::number_of(-1);
      });
    }
    if (key == "join") {
      return native_value("join", [target](const std::vector<Value>& arguments, Value) {
        const std::string separator = arguments.empty() ? std::string(",") : to_string(arguments[0]);
        return Value::string_of(join_values(target->elements, separator));
      });
    }
    if (key == "slice") {
      return native_value("slice", [target](const std::vector<Value>& arguments, Value) {
        const auto length = static_cast<long>(target->elements.size());
        long begin = arguments.empty() ? 0 : static_cast<long>(to_number(arguments[0]));
        long end = arguments.size() < 2 ? length : static_cast<long>(to_number(arguments[1]));
        if (begin < 0) begin = std::max(0L, length + begin);
        if (end < 0) end = std::max(0L, length + end);
        begin = std::min(begin, length);
        end = std::min(end, length);
        if (end < begin) end = begin;
        return Value::object_of(make_array(
            std::vector<Value>(target->elements.begin() + begin, target->elements.begin() + end)));
      });
    }
    if (key == "map" || key == "filter" || key == "forEach" || key == "find") {
      const std::string method = key;
      return native_value(method, [target, method](const std::vector<Value>& arguments, Value) {
        if (arguments.empty()) return Value::undefined();
        const Value callback = arguments[0];
        std::vector<Value> results;
        for (std::size_t i = 0; i < target->elements.size(); ++i) {
          const Value element = target->elements[i];
          const Value mapped = call_function(callback, Value::undefined(),
                                             {element, Value::number_of(static_cast<double>(i)),
                                              Value::object_of(target)});
          if (method == "map") {
            results.push_back(mapped);
          } else if (method == "filter" && to_boolean(mapped)) {
            results.push_back(element);
          } else if (method == "find" && to_boolean(mapped)) {
            return element;
          }
        }
        if (method == "forEach" || method == "find") return Value::undefined();
        return Value::object_of(make_array(std::move(results)));
      });
    }
    if (key == "reverse") {
      return native_value("reverse", [target](const std::vector<Value>&, Value) {
        std::reverse(target->elements.begin(), target->elements.end());
        return Value::object_of(target);
      });
    }
    if (key == "toString") {
      return native_value("toString", [target](const std::vector<Value>&, Value) {
        return Value::string_of(join_values(target->elements, ","));
      });
    }
    std::size_t index = 0;
    if (is_array_index(key, index)) {
      if (index < target->elements.size()) return target->elements[index];
      return Value::undefined();
    }
  }

  const auto found = target->properties.find(key);
  if (found != target->properties.end()) return found->second.value;
  const auto getter = target->getters.find(key);
  if (getter != target->getters.end()) return getter->second();
  if (target->prototype != nullptr) {
    const auto inherited = target->prototype->properties.find(key);
    if (inherited != target->prototype->properties.end()) return inherited->second.value;
  }
  return Value::undefined();
}

void Interpreter::set_member(const Value& object, const std::string& key, const Value& value) {
  if (!object.is_object() || object.object == nullptr) {
    throw thrown(type_error("cannot set property '" + key + "' of " + typeof_string(object)));
  }
  const std::shared_ptr<Object> target = object.object;
  if (target->is_array()) {
    if (key == "length") {
      target->elements.resize(static_cast<std::size_t>(std::max(0.0, to_number(value))));
      return;
    }
    std::size_t index = 0;
    if (is_array_index(key, index)) {
      if (index >= target->elements.size()) target->elements.resize(index + 1);
      target->elements[index] = value;
      return;
    }
  }
  const auto setter = target->setters.find(key);
  if (setter != target->setters.end()) {
    setter->second(value);
    return;
  }
  target->properties[key] = Property{value};
}

Value Interpreter::call(const Value& callee, const Value& this_value, std::vector<Value> arguments) {
  if (callee.type != ValueType::Function || callee.function == nullptr) {
    throw thrown(type_error(to_string(callee) + " is not a function"));
  }
  const std::shared_ptr<Function> function = callee.function;
  if (function->is_native()) return function->native(arguments, this_value);

  const auto code = std::static_pointer_cast<ast::FunctionNode>(function->code);
  const auto closure = static_cast<Scope*>(function->closure);
  const ScopePtr scope = runtime_.new_scope(closure);
  scope->is_function_scope = true;
  scope->this_value = this_value;
  for (std::size_t i = 0; i < code->parameters.size(); ++i) {
    scope->variables[code->parameters[i]] = i < arguments.size() ? arguments[i] : Value::undefined();
  }
  if (!code->is_arrow) scope->variables["arguments"] = Value::object_of(make_array(std::move(arguments)));
  if (!code->name.empty()) scope->variables[code->name] = callee;

  Interpreter* previous = g_interpreter;
  g_interpreter = this;
  for (const ast::StmtPtr& statement : code->body) {
    const Completion completion = exec(statement, scope);
    if (completion.type == CompletionType::Return) {
      g_interpreter = previous;
      return completion.value;
    }
    if (completion.type == CompletionType::Throw) {
      g_interpreter = previous;
      throw completion;
    }
    if (completion.type == CompletionType::Break || completion.type == CompletionType::Continue) break;
  }
  g_interpreter = previous;
  return Value::undefined();
}

Value call_function(const Value& callee, const Value& this_value, std::vector<Value> arguments) {
  if (g_interpreter == nullptr) return Value::undefined();
  return g_interpreter->call(callee, this_value, std::move(arguments));
}

Value Interpreter::make_function(const ast::FunctionPtr& node, const ScopePtr& scope) {
  auto function = std::make_shared<Function>();
  function->name = node->name;
  function->code = node;
  function->closure = const_cast<Scope*>(scope);
  function->parameter_count = node->parameters.size();
  if (!node->is_arrow) {
    function->prototype_object = std::make_shared<Object>();
    function->prototype_value = Value::object_of(function->prototype_object);
  }
  return Value::function_of(std::move(function));
}

// ------------------------------------------------------------- statements ----

Completion Interpreter::exec_statements(const std::vector<ast::StmtPtr>& statements, const ScopePtr& scope) {
  // A script's completion value is the value of its last expression statement,
  // which is what a console/CLI prints (spec: "script evaluation").
  Value last;
  for (const ast::StmtPtr& statement : statements) {
    const Completion completion = exec(statement, scope);
    if (completion.type != CompletionType::Normal) return completion;
    last = completion.value;
  }
  Completion completion;
  completion.value = std::move(last);
  return completion;
}

Completion Interpreter::exec_block(const std::vector<ast::StmtPtr>& statements, const ScopePtr& parent) {
  return exec_statements(statements, runtime_.new_scope(parent));
}

Completion Interpreter::exec(const ast::StmtPtr& statement, const ScopePtr& scope) {
  if (statement == nullptr) return normal();
  const ScopePtr current = scope;
  try {
    switch (statement->kind) {
      case ast::StmtKind::Empty:
        return normal();

      case ast::StmtKind::Expression: {
        Completion completion;
        completion.value = eval(statement->expression, current);
        return completion;
      }

      case ast::StmtKind::Var: {
        for (std::size_t i = 0; i < statement->names.size(); ++i) {
          const Value value = (i < statement->values.size() && statement->values[i] != nullptr)
                                  ? eval(statement->values[i], current)
                                  : Value::undefined();
          if (statement->declaration_kind == "var") {
            ScopePtr target = current;
            while (target->parent != nullptr && !target->is_function_scope) target = target->parent;
            target->variables[statement->names[i]] = value;
          } else {
            current->variables[statement->names[i]] = value;
          }
        }
        return normal();
      }

      case ast::StmtKind::Block:
        return exec_block(statement->statements, current);

      case ast::StmtKind::If:
        return to_boolean(eval(statement->expression, current)) ? exec(statement->body, current)
                                                                : exec(statement->alternate, current);

      case ast::StmtKind::While: {
        int iterations = 0;
        while (to_boolean(eval(statement->expression, current))) {
          const Completion completion = exec(statement->body, current);
          if (completion.type == CompletionType::Break) break;
          if (completion.type == CompletionType::Return || completion.type == CompletionType::Throw) return completion;
          if (++iterations > 200000) throw thrown(make_error("Error", "loop iteration limit exceeded"));
        }
        return normal();
      }

      case ast::StmtKind::DoWhile: {
        int iterations = 0;
        do {
          const Completion completion = exec(statement->body, current);
          if (completion.type == CompletionType::Break) break;
          if (completion.type == CompletionType::Return || completion.type == CompletionType::Throw) return completion;
          if (++iterations > 200000) throw thrown(make_error("Error", "loop iteration limit exceeded"));
        } while (to_boolean(eval(statement->expression, current)));
        return normal();
      }

      case ast::StmtKind::For: {
        const ScopePtr loop_scope = runtime_.new_scope(current);
        if (statement->initializer != nullptr) {
          const Completion init = exec(statement->initializer, loop_scope);
          if (init.type != CompletionType::Normal) return init;
        }
        int iterations = 0;
        for (;;) {
          if (statement->expression != nullptr && !to_boolean(eval(statement->expression, loop_scope))) break;
          const Completion completion = exec(statement->body, loop_scope);
          if (completion.type == CompletionType::Break) break;
          if (completion.type == CompletionType::Return || completion.type == CompletionType::Throw) return completion;
          if (statement->update_statement != nullptr) (void)exec(statement->update_statement, loop_scope);
          if (++iterations > 200000) throw thrown(make_error("Error", "loop iteration limit exceeded"));
        }
        return normal();
      }

      case ast::StmtKind::ForOf:
      case ast::StmtKind::ForIn: {
        const Value iterable = eval(statement->expression, current);
        std::vector<Value> items;
        if (statement->kind == ast::StmtKind::ForIn) {
          if (iterable.is_object() && iterable.object != nullptr) {
            std::vector<std::string> keys;
            for (const auto& entry : iterable.object->properties) keys.push_back(entry.first);
            std::sort(keys.begin(), keys.end());
            for (const std::string& entry : keys) items.push_back(Value::string_of(entry));
            if (iterable.object->is_array()) {
              for (std::size_t i = 0; i < iterable.object->elements.size(); ++i) {
                items.push_back(Value::string_of(std::to_string(i)));
              }
            }
          }
        } else if (iterable.is_string()) {
          for (const char c : iterable.text) items.push_back(Value::string_of(std::string(1, c)));
        } else if (iterable.is_object() && iterable.object != nullptr && iterable.object->is_array()) {
          items = iterable.object->elements;
        } else if (!iterable.is_nullish()) {
          throw thrown(type_error("value is not iterable"));
        }

        for (const Value& item : items) {
          const ScopePtr iteration_scope = runtime_.new_scope(current);
          if (statement->declaration_kind == "var") {
            current->variables[statement->names.front()] = item;
          } else if (!statement->names.empty()) {
            iteration_scope->variables[statement->names.front()] = item;
          } else if (!statement->values.empty() && statement->values.front() != nullptr) {
            const ast::ExprPtr& target = statement->values.front();
            if (target->kind == ast::ExprKind::Identifier) {
              const Lookup found = lookup_scope(current, target->text);
              if (found.found) found.scope->variables[target->text] = item;
              else current->variables[target->text] = item;
            } else if (target->kind == ast::ExprKind::Member) {
              const Value object = eval(target->children[0], current);
              const std::string key = target->boolean ? to_string(eval(target->children[1], current)) : target->text;
              set_member(object, key, item);
            }
          }
          const Completion completion = exec(statement->body, iteration_scope);
          if (completion.type == CompletionType::Break) break;
          if (completion.type == CompletionType::Return || completion.type == CompletionType::Throw) return completion;
        }
        return normal();
      }

      case ast::StmtKind::FunctionDecl: {
        const Value function = make_function(statement->function, current);
        current->variables[statement->function->name] = function;
        return normal();
      }

      case ast::StmtKind::Return: {
        Completion completion;
        completion.type = CompletionType::Return;
        completion.value = statement->expression != nullptr ? eval(statement->expression, current) : Value::undefined();
        return completion;
      }

      case ast::StmtKind::Break: {
        Completion completion;
        completion.type = CompletionType::Break;
        return completion;
      }

      case ast::StmtKind::Continue: {
        Completion completion;
        completion.type = CompletionType::Continue;
        return completion;
      }

      case ast::StmtKind::Throw:
        return thrown(eval(statement->expression, current));

      case ast::StmtKind::Try: {
        Completion completion = exec(statement->try_block, current);
        if (completion.type == CompletionType::Throw && statement->catch_block != nullptr) {
          const ScopePtr catch_scope = runtime_.new_scope(current);
          if (!statement->catch_name.empty()) catch_scope->variables[statement->catch_name] = completion.value;
          completion = exec_statements(statement->catch_block->statements, catch_scope);
        }
        if (statement->finally_block != nullptr) {
          const Completion final_completion = exec(statement->finally_block, current);
          if (final_completion.type != CompletionType::Normal) return final_completion;
        }
        return completion;
      }
    }
  } catch (const Completion& completion) {
    return completion;
  }
  return normal();
}

// ------------------------------------------------------------ expressions ----

Value Interpreter::eval_call(const ast::Expr& expression, const ScopePtr& scope) {
  const ast::ExprPtr& callee_expression = expression.children[0];
  Value this_value = Value::undefined();
  Value callee;

  if (callee_expression->kind == ast::ExprKind::Member) {
    const Value object = eval(callee_expression->children[0], scope);
    const std::string key =
        callee_expression->boolean ? to_string(eval(callee_expression->children[1], scope)) : callee_expression->text;
    this_value = object;
    callee = get_member(object, key);
  } else {
    callee = eval(callee_expression, scope);
  }

  std::vector<Value> arguments;
  arguments.reserve(expression.children.size());
  for (std::size_t i = 1; i < expression.children.size(); ++i) {
    arguments.push_back(eval(expression.children[i], scope));
  }
  if (callee.type != ValueType::Function) throw thrown(type_error(to_string(callee) + " is not a function"));
  return call(callee, this_value, std::move(arguments));
}

Value Interpreter::eval(const ast::ExprPtr& expression, const ScopePtr& scope) {
  if (expression == nullptr) return Value::undefined();
  switch (expression->kind) {
    case ast::ExprKind::Number:
      return Value::number_of(expression->number);
    case ast::ExprKind::String:
      return Value::string_of(expression->text);
    case ast::ExprKind::Boolean:
      return Value::boolean_of(expression->boolean);
    case ast::ExprKind::Null:
      return Value::null();
    case ast::ExprKind::Undefined:
      return Value::undefined();
    case ast::ExprKind::This:
      return scope->this_value;

    case ast::ExprKind::Identifier: {
      const Lookup found = lookup_scope(scope, expression->text);
      if (found.found) return found.scope->variables[expression->text];
      return Value::undefined();
    }

    case ast::ExprKind::Array: {
      std::vector<Value> elements;
      elements.reserve(expression->children.size());
      for (const ast::ExprPtr& element : expression->children) elements.push_back(eval(element, scope));
      return Value::object_of(make_array(std::move(elements)));
    }

    case ast::ExprKind::ObjectLiteral: {
      auto object = std::make_shared<Object>();
      for (const ast::PropertyDef& property : expression->properties) {
        object->properties[property.key] = Property{eval(property.value, scope)};
      }
      return Value::object_of(std::move(object));
    }

    case ast::ExprKind::Function:
      return make_function(expression->function, scope);

    case ast::ExprKind::Sequence: {
      Value last;
      for (const ast::ExprPtr& child : expression->children) last = eval(child, scope);
      return last;
    }

    case ast::ExprKind::Template: {
      std::string out;
      for (const ast::ExprPtr& part : expression->children) {
        out += part->kind == ast::ExprKind::String ? part->text : to_string(eval(part, scope));
      }
      return Value::string_of(out);
    }

    case ast::ExprKind::Unary: {
      const std::string& op = expression->text;
      if (op == "delete") {
        const ast::ExprPtr& target = expression->children[0];
        if (target->kind == ast::ExprKind::Member) {
          const Value object = eval(target->children[0], scope);
          const std::string key = target->boolean ? to_string(eval(target->children[1], scope)) : target->text;
          if (object.is_object() && object.object != nullptr) object.object->properties.erase(key);
        }
        return Value::boolean_of(true);
      }
      const Value operand = eval(expression->children[0], scope);
      if (op == "typeof") return Value::string_of(typeof_string(operand));
      if (op == "!") return Value::boolean_of(!to_boolean(operand));
      if (op == "-") return Value::number_of(-to_number(operand));
      if (op == "+") return Value::number_of(to_number(operand));
      if (op == "~") return Value::number_of(static_cast<double>(~static_cast<long long>(to_number(operand))));
      return Value::undefined();
    }

    case ast::ExprKind::Update: {
      const ast::ExprPtr& target = expression->children[0];
      const double delta = expression->text == "++" ? 1.0 : -1.0;
      Value old_value;
      if (target->kind == ast::ExprKind::Identifier) {
        const Lookup found = lookup_scope(scope, target->text);
        old_value = found.found ? found.scope->variables[target->text] : Value::undefined();
      } else if (target->kind == ast::ExprKind::Member) {
        const Value object = eval(target->children[0], scope);
        const std::string key = target->boolean ? to_string(eval(target->children[1], scope)) : target->text;
        old_value = get_member(object, key);
      }
      const Value new_value = Value::number_of(to_number(old_value) + delta);
      if (target->kind == ast::ExprKind::Identifier) {
        const Lookup found = lookup_scope(scope, target->text);
        if (found.found) found.scope->variables[target->text] = new_value;
        else scope->variables[target->text] = new_value;
      } else if (target->kind == ast::ExprKind::Member) {
        const Value object = eval(target->children[0], scope);
        const std::string key = target->boolean ? to_string(eval(target->children[1], scope)) : target->text;
        set_member(object, key, new_value);
      }
      return expression->boolean ? new_value : old_value;
    }

    case ast::ExprKind::Logical: {
      const Value left = eval(expression->children[0], scope);
      if (expression->text == "&&") return to_boolean(left) ? eval(expression->children[1], scope) : left;
      if (expression->text == "||") return to_boolean(left) ? left : eval(expression->children[1], scope);
      return left.is_nullish() ? eval(expression->children[1], scope) : left;
    }

    case ast::ExprKind::Conditional:
      return to_boolean(eval(expression->children[0], scope)) ? eval(expression->children[1], scope)
                                                             : eval(expression->children[2], scope);

    case ast::ExprKind::Binary: {
      const std::string& op = expression->text;
      const Value left = eval(expression->children[0], scope);
      const Value right = eval(expression->children[1], scope);
      if (op == "+") {
        if (left.is_string() || right.is_string()) return Value::string_of(to_string(left) + to_string(right));
        return Value::number_of(to_number(left) + to_number(right));
      }
      if (op == "-") return Value::number_of(to_number(left) - to_number(right));
      if (op == "*") return Value::number_of(to_number(left) * to_number(right));
      if (op == "/") return Value::number_of(to_number(left) / to_number(right));
      if (op == "%") return Value::number_of(std::fmod(to_number(left), to_number(right)));
      if (op == "**") return Value::number_of(std::pow(to_number(left), to_number(right)));
      if (op == "==") return Value::boolean_of(loose_equals(left, right));
      if (op == "!=") return Value::boolean_of(!loose_equals(left, right));
      if (op == "===") return Value::boolean_of(strict_equals(left, right));
      if (op == "!==") return Value::boolean_of(!strict_equals(left, right));
      if (op == "<") return Value::boolean_of(to_number(left) < to_number(right));
      if (op == ">") return Value::boolean_of(to_number(left) > to_number(right));
      if (op == "<=") return Value::boolean_of(to_number(left) <= to_number(right));
      if (op == ">=") return Value::boolean_of(to_number(left) >= to_number(right));
      if (op == "&") {
        return Value::number_of(static_cast<double>(static_cast<long long>(to_number(left)) &
                                                    static_cast<long long>(to_number(right))));
      }
      if (op == "|") {
        return Value::number_of(static_cast<double>(static_cast<long long>(to_number(left)) |
                                                    static_cast<long long>(to_number(right))));
      }
      if (op == "^") {
        return Value::number_of(static_cast<double>(static_cast<long long>(to_number(left)) ^
                                                    static_cast<long long>(to_number(right))));
      }
      if (op == "<<") {
        return Value::number_of(static_cast<double>(static_cast<long long>(to_number(left))
                                                    << (static_cast<long long>(to_number(right)) & 31)));
      }
      if (op == ">>") {
        return Value::number_of(static_cast<double>(static_cast<long long>(to_number(left)) >>
                                                    (static_cast<long long>(to_number(right)) & 31)));
      }
      if (op == "in") {
        const std::string key = to_string(left);
        if (!right.is_object() || right.object == nullptr) return Value::boolean_of(false);
        std::size_t index = 0;
        if (right.object->is_array() && is_array_index(key, index) && index < right.object->elements.size()) {
          return Value::boolean_of(true);
        }
        return Value::boolean_of(right.object->properties.find(key) != right.object->properties.end());
      }
      if (op == "instanceof") {
        if (!right.is_object() || !left.is_object() || left.object == nullptr) return Value::boolean_of(false);
        std::shared_ptr<Object> prototype;
        if (right.type == ValueType::Function) {
          prototype = right.function != nullptr ? right.function->prototype_object : nullptr;
        } else {
          prototype = right.object;
        }
        if (prototype == nullptr) return Value::boolean_of(false);
        std::shared_ptr<Object> current = left.object->prototype;
        while (current != nullptr) {
          if (current == prototype) return Value::boolean_of(true);
          current = current->prototype;
        }
        return Value::boolean_of(false);
      }
      return Value::undefined();
    }

    case ast::ExprKind::Assign: {
      const ast::ExprPtr& target = expression->children[0];
      Value value = eval(expression->children[1], scope);
      if (expression->text != "=") {
        Value current_value;
        if (target->kind == ast::ExprKind::Identifier) {
          const Lookup found = lookup_scope(scope, target->text);
          current_value = found.found ? found.scope->variables[target->text] : Value::undefined();
        } else if (target->kind == ast::ExprKind::Member) {
          const Value object = eval(target->children[0], scope);
          const std::string key = target->boolean ? to_string(eval(target->children[1], scope)) : target->text;
          current_value = get_member(object, key);
        }
        const std::string op = expression->text.substr(0, expression->text.size() - 1);
        if (op == "+" && (current_value.is_string() || value.is_string())) {
          value = Value::string_of(to_string(current_value) + to_string(value));
        } else if (op == "+") {
          value = Value::number_of(to_number(current_value) + to_number(value));
        } else if (op == "-") {
          value = Value::number_of(to_number(current_value) - to_number(value));
        } else if (op == "*") {
          value = Value::number_of(to_number(current_value) * to_number(value));
        } else if (op == "/") {
          value = Value::number_of(to_number(current_value) / to_number(value));
        } else if (op == "%") {
          value = Value::number_of(std::fmod(to_number(current_value), to_number(value)));
        }
      }

      if (target->kind == ast::ExprKind::Identifier) {
        const Lookup found = lookup_scope(scope, target->text);
        if (found.found) found.scope->variables[target->text] = value;
        else scope->variables[target->text] = value;
        return value;
      }
      if (target->kind == ast::ExprKind::Member) {
        const Value object = eval(target->children[0], scope);
        const std::string key = target->boolean ? to_string(eval(target->children[1], scope)) : target->text;
        set_member(object, key, value);
        return value;
      }
      throw thrown(reference_error("invalid assignment target"));
    }

    case ast::ExprKind::Member: {
      const Value object = eval(expression->children[0], scope);
      const std::string key = expression->boolean ? to_string(eval(expression->children[1], scope)) : expression->text;
      return get_member(object, key);
    }

    case ast::ExprKind::Call:
      return eval_call(*expression, scope);

    case ast::ExprKind::New: {
      const Value constructor = eval(expression->children[0], scope);
      if (constructor.type != ValueType::Function) throw thrown(type_error("not a constructor"));
      auto instance = std::make_shared<Object>();
      if (constructor.function->prototype_object != nullptr) instance->prototype = constructor.function->prototype_object;
      std::vector<Value> arguments;
      for (std::size_t i = 1; i < expression->children.size(); ++i) {
        arguments.push_back(eval(expression->children[i], scope));
      }
      const Value result = call(constructor, Value::object_of(instance), std::move(arguments));
      if (result.type == ValueType::Object && result.object != nullptr) return result;
      return Value::object_of(std::move(instance));
    }
  }
  return Value::undefined();
}

// --------------------------------------------------------------- builtins ----

void install_builtins(RuntimeState& runtime) {
  const ScopePtr global = runtime.global;

  global->variables["undefined"] = Value::undefined();
  global->variables["NaN"] = Value::number_of(std::nan(""));
  global->variables["Infinity"] = Value::number_of(INFINITY);

  const Value console_object = Value::object_of(std::make_shared<Object>());
  auto add_console = [&](const std::string& name, ConsoleEntry::Level level) {
    console_object.object->properties[name] = Property{native_value(
        name, [&runtime, level, name](const std::vector<Value>& arguments, Value) {
          std::string text;
          for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i != 0) text += ' ';
            const Value& argument = arguments[i];
            if (argument.type == ValueType::Object && argument.object != nullptr) {
              text += json_serialize(argument, 0);
            } else {
              text += to_string(argument);
            }
          }
          runtime.log(level, text);
          return Value::undefined();
        })};
  };
  add_console("log", ConsoleEntry::Level::Log);
  add_console("info", ConsoleEntry::Level::Log);
  add_console("warn", ConsoleEntry::Level::Warn);
  add_console("error", ConsoleEntry::Level::Error);
  global->variables["console"] = console_object;

  const Value math = Value::object_of(std::make_shared<Object>());
  math.object->properties["PI"] = Property{Value::number_of(3.141592653589793)};
  math.object->properties["E"] = Property{Value::number_of(2.718281828459045)};
  auto add_math = [&](const std::string& name, Function::Native native) {
    math.object->properties[name] = Property{native_value(name, std::move(native))};
  };
  add_math("round", [](const std::vector<Value>& arguments, Value) {
    return Value::number_of(arguments.empty() ? 0.0 : std::round(to_number(arguments[0])));
  });
  add_math("floor", [](const std::vector<Value>& arguments, Value) {
    return Value::number_of(arguments.empty() ? 0.0 : std::floor(to_number(arguments[0])));
  });
  add_math("ceil", [](const std::vector<Value>& arguments, Value) {
    return Value::number_of(arguments.empty() ? 0.0 : std::ceil(to_number(arguments[0])));
  });
  add_math("abs", [](const std::vector<Value>& arguments, Value) {
    return Value::number_of(arguments.empty() ? 0.0 : std::fabs(to_number(arguments[0])));
  });
  add_math("max", [](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::number_of(-INFINITY);
    double best = to_number(arguments[0]);
    for (std::size_t i = 1; i < arguments.size(); ++i) best = std::max(best, to_number(arguments[i]));
    return Value::number_of(best);
  });
  add_math("min", [](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::number_of(INFINITY);
    double best = to_number(arguments[0]);
    for (std::size_t i = 1; i < arguments.size(); ++i) best = std::min(best, to_number(arguments[i]));
    return Value::number_of(best);
  });
  add_math("pow", [](const std::vector<Value>& arguments, Value) {
    if (arguments.size() < 2) return Value::number_of(std::nan(""));
    return Value::number_of(std::pow(to_number(arguments[0]), to_number(arguments[1])));
  });
  add_math("sqrt", [](const std::vector<Value>& arguments, Value) {
    return Value::number_of(arguments.empty() ? std::nan("") : std::sqrt(to_number(arguments[0])));
  });
  add_math("random", [](const std::vector<Value>&, Value) {
    return Value::number_of(static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX));
  });
  global->variables["Math"] = math;

  const Value json = Value::object_of(std::make_shared<Object>());
  json.object->properties["stringify"] = Property{native_value("stringify", [](const std::vector<Value>& arguments, Value) {
                                                    if (arguments.empty()) return Value::undefined();
                                                    return Value::string_of(json_serialize(arguments[0], 0));
                                                  })};
  json.object->properties["parse"] = Property{native_value("parse", [](const std::vector<Value>& arguments, Value) {
                                                if (arguments.empty()) return Value::undefined();
                                                // JsonReader holds a string_view, so the string must outlive it.
                                                const std::string text = to_string(arguments[0]);
                                                JsonReader reader{text, 0};
                                                return reader.parse();
                                              })};
  global->variables["JSON"] = json;

  global->variables["parseInt"] = native_value("parseInt", [](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::number_of(std::nan(""));
    const std::string text = to_string(arguments[0]);
    int base = arguments.size() > 1 ? static_cast<int>(to_number(arguments[1])) : 10;
    if (base == 0) base = 10;
    return Value::number_of(static_cast<double>(std::strtol(text.c_str(), nullptr, base)));
  });
  global->variables["parseFloat"] = native_value("parseFloat", [](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::number_of(std::nan(""));
    return Value::number_of(std::strtod(to_string(arguments[0]).c_str(), nullptr));
  });
  global->variables["isNaN"] = native_value("isNaN", [](const std::vector<Value>& arguments, Value) {
    return Value::boolean_of(arguments.empty() || std::isnan(to_number(arguments[0])));
  });

  global->variables["Number"] = native_value("Number", [](const std::vector<Value>& arguments, Value) {
    return Value::number_of(arguments.empty() ? 0.0 : to_number(arguments[0]));
  });
  global->variables["String"] = native_value("String", [](const std::vector<Value>& arguments, Value) {
    return Value::string_of(arguments.empty() ? std::string() : to_string(arguments[0]));
  });
  global->variables["Boolean"] = native_value("Boolean", [](const std::vector<Value>& arguments, Value) {
    return Value::boolean_of(!arguments.empty() && to_boolean(arguments[0]));
  });
  global->variables["Array"] = native_value("Array", [](const std::vector<Value>& arguments, Value) {
    return Value::object_of(make_array(arguments));
  });
  global->variables["Object"] = native_value("Object", [](const std::vector<Value>& arguments, Value) {
    if (!arguments.empty() && arguments[0].is_object()) return arguments[0];
    return Value::object_of(std::make_shared<Object>());
  });

  // setTimeout(fn, ms): callbacks run when the host pumps them (0 ms only).
  global->variables["setTimeout"] = native_value("setTimeout", [&runtime](const std::vector<Value>& arguments, Value) {
    if (!arguments.empty()) runtime.timer_queue.push_back(arguments[0]);
    return Value::number_of(0);
  });
  global->variables["clearTimeout"] = native_value("clearTimeout", [](const std::vector<Value>&, Value) {
    return Value::undefined();
  });
  global->variables["alert"] = native_value("alert", [&runtime](const std::vector<Value>& arguments, Value) {
    runtime.log(ConsoleEntry::Level::Warn, "[alert] " + (arguments.empty() ? std::string() : to_string(arguments[0])));
    return Value::undefined();
  });

  // Error family. `new X(message)` receives a fresh instance as `this`, so the
  // native constructor fills that object instead of allocating a second one.
  auto error_constructor = [](const std::string& error_name) {
    return native_value(error_name, [error_name](const std::vector<Value>& arguments, Value this_value) {
      std::shared_ptr<Object> error = (this_value.type == ValueType::Object && this_value.object != nullptr)
                                          ? this_value.object
                                          : std::make_shared<Object>();
      error->kind = Object::Kind::Error;
      const std::string message = arguments.empty() ? std::string() : to_string(arguments[0]);
      error->properties["name"] = Property{Value::string_of(error_name)};
      error->properties["message"] = Property{Value::string_of(message)};
      error->properties["stack"] =
          Property{Value::string_of(error_name + (message.empty() ? std::string() : ": " + message))};
      return Value::object_of(error);
    });
  };
  for (const char* name : {"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "URIError"}) {
    global->variables[name] = error_constructor(name);
  }

  global->variables["globalThis"] = Value::object_of(std::make_shared<Object>());
}

}  // namespace

// ------------------------------------------------------------- public API ----

// ------------------------------------------------------ public coercions ----

bool to_boolean(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
    case ValueType::Null:
      return false;
    case ValueType::Boolean:
      return value.boolean;
    case ValueType::Number:
      return value.number != 0.0 && !std::isnan(value.number);
    case ValueType::String:
      return !value.text.empty();
    case ValueType::Object:
    case ValueType::Function:
      return true;
  }
  return false;
}

double to_number(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
      return std::nan("");
    case ValueType::Null:
      return 0.0;
    case ValueType::Boolean:
      return value.boolean ? 1.0 : 0.0;
    case ValueType::Number:
      return value.number;
    case ValueType::String: {
      const std::string& text = value.text;
      const std::size_t begin = text.find_first_not_of(" \t\n\r");
      if (begin == std::string::npos) return 0.0;
      const char* start = text.c_str() + begin;
      char* end = nullptr;
      const double parsed = std::strtod(start, &end);
      if (end == start) return std::nan("");
      return parsed;
    }
    case ValueType::Object:
    case ValueType::Function:
      return std::nan("");
  }
  return std::nan("");
}

std::string to_string(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
      return "undefined";
    case ValueType::Null:
      return "null";
    case ValueType::Boolean:
      return value.boolean ? "true" : "false";
    case ValueType::Number:
      return number_to_raw_string(value.number);
    case ValueType::String:
      return value.text;
    case ValueType::Function:
      return "function " + (value.function != nullptr ? value.function->name : std::string()) + "() { [code] }";
    case ValueType::Object: {
      if (value.object == nullptr) return "null";
      const Object& object = *value.object;
      if (object.is_array()) {
        std::string out;
        for (std::size_t i = 0; i < object.elements.size(); ++i) {
          if (i != 0) out += ',';
          out += object.elements[i].is_nullish() ? std::string() : to_string(object.elements[i]);
        }
        return out;
      }
      if (object.kind == Object::Kind::Error) {
        const auto name = object.properties.find("name");
        const auto message = object.properties.find("message");
        std::string out = name != object.properties.end() ? to_string(name->second.value) : "Error";
        if (message != object.properties.end()) out += ": " + to_string(message->second.value);
        return out;
      }
      return "[object Object]";
    }
  }
  return std::string();
}

std::string typeof_string(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
      return "undefined";
    case ValueType::Null:
      return "object";
    case ValueType::Boolean:
      return "boolean";
    case ValueType::Number:
      return "number";
    case ValueType::String:
      return "string";
    case ValueType::Function:
      return "function";
    case ValueType::Object:
      return "object";
  }
  return "undefined";
}

std::string number_to_string(double value) { return number_to_raw_string(value); }

std::string describe_exception(const Value& value) {
  if (value.type == ValueType::Object && value.object != nullptr) {
    const auto message = value.object->properties.find("message");
    if (message != value.object->properties.end()) return to_string(message->second.value);
  }
  return to_string(value);
}

bool strict_equals(const Value& a, const Value& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case ValueType::Undefined:
    case ValueType::Null:
      return true;
    case ValueType::Boolean:
      return a.boolean == b.boolean;
    case ValueType::Number:
      return a.number == b.number;
    case ValueType::String:
      return a.text == b.text;
    case ValueType::Object:
      return a.object == b.object;
    case ValueType::Function:
      return a.function == b.function;
  }
  return false;
}

bool loose_equals(const Value& a, const Value& b) {
  if (a.type == b.type) return strict_equals(a, b);
  if (a.is_nullish() && b.is_nullish()) return true;
  if (a.is_nullish() || b.is_nullish()) return false;
  if (a.type == ValueType::Boolean) return loose_equals(Value::number_of(to_number(a)), b);
  if (b.type == ValueType::Boolean) return loose_equals(a, Value::number_of(to_number(b)));
  if (a.is_number() && b.is_string()) return a.number == to_number(b);
  if (a.is_string() && b.is_number()) return to_number(a) == b.number;
  if (a.type == ValueType::Object && b.is_string()) return to_string(a) == b.text;
  if (a.is_string() && b.type == ValueType::Object) return a.text == to_string(b);
  if (a.type == ValueType::Object && b.is_number()) return to_number(a) == b.number;
  if (a.is_number() && b.type == ValueType::Object) return a.number == to_number(b);
  return false;
}

struct Runtime::Impl {
  RuntimeState* state = nullptr;
  Interpreter* interpreter = nullptr;
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;

Value Runtime::call(const Value& callee, std::vector<Value> arguments) {
  if (impl_->state == nullptr || impl_->interpreter == nullptr) return Value::undefined();
  Interpreter* previous = g_interpreter;
  g_interpreter = impl_->interpreter;
  Value result = Value::undefined();
  try {
    result = impl_->interpreter->call(callee, Value::undefined(), std::move(arguments));
  } catch (const Completion&) {
    result = Value::undefined();
  }
  g_interpreter = previous;
  return result;
}

Value Runtime::global(std::string_view name) const {
  if (impl_->state == nullptr) return Value::undefined();
  const auto found = impl_->state->global->variables.find(std::string(name));
  return found == impl_->state->global->variables.end() ? Value::undefined() : found->second;
}

void Runtime::set_global(std::string name, Value value) {
  if (impl_->state != nullptr) impl_->state->global->variables[std::move(name)] = std::move(value);
}

void Runtime::log(ConsoleEntry::Level level, std::string message) {
  if (impl_->state != nullptr) impl_->state->log(level, std::move(message));
}

const std::vector<ConsoleEntry>& Runtime::console() const {
  static const std::vector<ConsoleEntry> empty;
  return impl_->state == nullptr ? empty : impl_->state->console;
}

const std::unordered_map<std::string, std::function<Value(const std::vector<Value>&)>>& Runtime::host_callbacks()
    const {
  static const std::unordered_map<std::string, std::function<Value(const std::vector<Value>&)>> empty;
  return impl_->state == nullptr ? empty : impl_->state->host_callbacks;
}

struct Context::Impl {
  RuntimeState runtime;
  std::unique_ptr<Interpreter> interpreter;
  Runtime public_runtime;
};

Context::Context() : impl_(std::make_unique<Impl>()) {
  impl_->runtime.global = impl_->runtime.new_scope(nullptr);
  impl_->runtime.global->is_function_scope = true;
  install_builtins(impl_->runtime);
  impl_->interpreter = std::make_unique<Interpreter>(impl_->runtime, impl_->runtime.global);
  impl_->public_runtime.impl_->state = &impl_->runtime;
  impl_->public_runtime.impl_->interpreter = impl_->interpreter.get();
}

Context::~Context() = default;

Runtime& Context::runtime() { return impl_->public_runtime; }

void Context::set_global(std::string name, Value value) { impl_->runtime.global->variables[name] = std::move(value); }

void Context::set_host_callback(std::string name, std::function<Value(const std::vector<Value>&)> callback) {
  impl_->runtime.host_callbacks[name] = std::move(callback);
}

const std::vector<ConsoleEntry>& Context::console() const noexcept { return impl_->runtime.console; }

void Context::clear_console() { impl_->runtime.console.clear(); }

ExecutionResult Context::evaluate(std::string_view source, std::string_view origin) {
  impl_->runtime.origin = std::string(origin);

  Result<ast::StmtPtr> program = parse_script(source);
  if (program.is_error()) {
    ExecutionResult result;
    result.ok = false;
    result.message = program.error().to_string();
    result.exception = make_error("SyntaxError", program.error().message);
    return result;
  }

  Interpreter* previous = g_interpreter;
  g_interpreter = impl_->interpreter.get();

  ExecutionResult result;
  try {
    const Completion completion = impl_->interpreter->exec_statements(program.value()->statements,
                                                                      impl_->runtime.global);
    if (completion.type == CompletionType::Throw) {
      result.ok = false;
      result.exception = completion.value;
      result.message = "Uncaught " + describe_exception(completion.value);
    } else {
      result.value = completion.value;
    }
  } catch (const Completion& completion) {
    result.ok = false;
    result.exception = completion.value;
    result.message = "Uncaught " + describe_exception(completion.value);
  } catch (const std::exception& error) {
    result.ok = false;
    result.message = std::string("js.runtime: ") + error.what();
    result.exception = make_error("Error", error.what());
  }

  g_interpreter = previous;
  return result;
}

void Context::run_pending_callbacks() {
  std::vector<Value> queue = impl_->runtime.timer_queue;
  impl_->runtime.timer_queue.clear();
  Interpreter* previous = g_interpreter;
  g_interpreter = impl_->interpreter.get();
  for (const Value& callback : queue) {
    try {
      (void)impl_->interpreter->call(callback, Value::undefined(), {});
    } catch (const Completion&) {
      // Timer exceptions are swallowed, like a browser's "uncaught error in timer".
    }
  }
  g_interpreter = previous;
}

}  // namespace bonk::js
