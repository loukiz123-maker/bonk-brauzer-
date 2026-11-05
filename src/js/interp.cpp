// Tree-walking interpreter for the Bonk Brauzer JS engine.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
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
  /// The one Object.prototype every plain object inherits from (stage 23).
  std::shared_ptr<Object> object_prototype;

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

// ------------------------------------------------------ own properties ----

Value call_function(const Value& callee, const Value& this_value, std::vector<Value> arguments);

/// The property bag of an object *or function* value: functions are objects and
/// carry their own properties in `Function::statics` (stage 23).
std::shared_ptr<Object> property_bag(const Value& value) {
  if (value.type == ValueType::Object) return value.object;
  if (value.type == ValueType::Function && value.function != nullptr) {
    if (value.function->statics == nullptr) value.function->statics = std::make_shared<Object>();
    return value.function->statics;
  }
  return nullptr;
}

/// True when `object` has an accessor (get/set) property named `key`.
bool has_accessor(const Object& object, const std::string& key) {
  return object.getters.count(key) != 0 || object.setters.count(key) != 0 ||
         object.receiver_getters.count(key) != 0 || object.receiver_setters.count(key) != 0;
}

/// Own property keys. Array indices come first in ascending numeric order, then
/// the remaining string keys sorted: `std::unordered_map` has no insertion
/// order, so we sort instead of pretending to preserve it (see ADR 0025).
std::vector<std::string> own_property_keys(const Object& object, bool enumerable_only) {
  std::vector<std::string> indices;
  std::vector<std::string> names;
  auto classify = [&](const std::string& key, bool enumerable) {
    if (enumerable_only && !enumerable) return;
    std::size_t index = 0;
    (is_array_index(key, index) ? indices : names).push_back(key);
  };
  for (const auto& entry : object.properties) classify(entry.first, entry.second.enumerable);
  auto classify_accessors = [&](const auto& map) {
    for (const auto& entry : map) {
      const auto property = object.properties.find(entry.first);
      const bool enumerable = property == object.properties.end() || property->second.enumerable;
      classify(entry.first, enumerable || !enumerable_only);
    }
  };
  classify_accessors(object.getters);
  classify_accessors(object.setters);
  classify_accessors(object.receiver_getters);
  classify_accessors(object.receiver_setters);
  if (object.is_array()) {
    for (std::size_t i = 0; i < object.elements.size(); ++i) indices.push_back(std::to_string(i));
  }
  auto numeric_less = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
  };
  std::sort(indices.begin(), indices.end(), numeric_less);
  std::sort(names.begin(), names.end());
  indices.insert(indices.end(), names.begin(), names.end());
  return indices;
}

/// Property lookup that follows the whole prototype chain.
bool lookup_property(const std::shared_ptr<Object>& start, const std::string& key, Value& out) {
  for (std::shared_ptr<Object> ancestor = start; ancestor != nullptr; ancestor = ancestor->prototype) {
    const auto receiver_getter = ancestor->receiver_getters.find(key);
    if (receiver_getter != ancestor->receiver_getters.end()) {
      out = receiver_getter->second(Value::object_of(ancestor));
      return true;
    }
    const auto getter = ancestor->getters.find(key);
    if (getter != ancestor->getters.end()) {
      out = getter->second();
      return true;
    }
    const auto found = ancestor->properties.find(key);
    if (found != ancestor->properties.end()) {
      out = found->second.value;
      return true;
    }
    if (ancestor->is_array()) {
      std::size_t index = 0;
      if (is_array_index(key, index) && index < ancestor->elements.size()) {
        out = ancestor->elements[index];
        return true;
      }
    }
    if (key == "constructor") {
      if (const std::shared_ptr<Function> owner = ancestor->owner_function.lock()) {
        out = Value::function_of(owner);
        return true;
      }
    }
  }
  return false;
}

/// [[DefineOwnProperty]] for data/accessor descriptors, with the spec's
/// non-configurable checks (ES2015 9.1.6.3, the parts tests actually probe).
void define_own_property(const std::shared_ptr<Object>& object, const std::string& key,
                         const std::optional<Value>& data_value, const std::optional<Value>& getter,
                         const std::optional<Value>& setter, std::optional<bool> writable,
                         std::optional<bool> enumerable, std::optional<bool> configurable) {
  const auto existing = object->properties.find(key);
  const bool is_accessor = has_accessor(*object, key);
  if (existing != object->properties.end()) {
    const Property& property = existing->second;
    if (!property.configurable) {
      if (configurable.value_or(false)) throw thrown(type_error("cannot redefine property: " + key));
      if (enumerable.has_value() && *enumerable != property.enumerable) {
        throw thrown(type_error("cannot redefine property: " + key));
      }
      const bool wants_accessor = getter.has_value() || setter.has_value();
      if (wants_accessor != is_accessor) throw thrown(type_error("cannot redefine property: " + key));
      if (!is_accessor && !property.writable) {
        if (writable.value_or(false)) throw thrown(type_error("cannot redefine property: " + key));
        if (data_value.has_value() && !strict_equals(*data_value, property.value)) {
          throw thrown(type_error("cannot redefine property: " + key));
        }
      }
    }
  } else {
    if (!object->extensible) throw thrown(type_error("object is not extensible"));
    object->properties[key] = Property{Value::undefined(), true, true, true};
  }

  Property& property = object->properties[key];
  if (enumerable.has_value()) property.enumerable = *enumerable;
  if (configurable.has_value()) property.configurable = *configurable;
  if (getter.has_value()) {
    object->getters.erase(key);
    if (getter->is_undefined()) {
      object->receiver_getters.erase(key);
    } else {
      const Value captured = *getter;
      if (captured.type != ValueType::Function) throw thrown(type_error("getter must be a function"));
      object->receiver_getters[key] = [captured](const Value& self) {
        return call_function(captured, self, {});
      };
    }
  }
  if (setter.has_value()) {
    object->setters.erase(key);
    if (setter->is_undefined()) {
      object->receiver_setters.erase(key);
    } else {
      const Value captured = *setter;
      if (captured.type != ValueType::Function) throw thrown(type_error("setter must be a function"));
      object->receiver_setters[key] = [captured](const Value& self, const Value& value) {
        call_function(captured, self, {value});
      };
    }
  }
  if (!is_accessor && data_value.has_value()) property.value = *data_value;
  if (writable.has_value()) property.writable = *writable;
}

/// Does `object` own a property (data, accessor or array element)?
bool has_own_property(const Object& object, const std::string& key) {
  if (key == "constructor" && !object.owner_function.expired()) return true;
  if (has_accessor(object, key)) return true;
  if (object.properties.count(key) != 0) return true;
  std::size_t index = 0;
  return object.is_array() && is_array_index(key, index) && index < object.elements.size();
}

/// The descriptor object `Object.getOwnPropertyDescriptor` hands back.
Value property_descriptor(const Object& object, const std::string& key) {
  if (!has_own_property(object, key)) return Value::undefined();
  auto descriptor = std::make_shared<Object>();
  auto put = [&descriptor](const char* name, Value value, bool enumerable) {
    Property property{std::move(value)};
    property.enumerable = enumerable;
    descriptor->properties[name] = property;
  };
  const auto stored = object.properties.find(key);
  const bool enumerable = stored == object.properties.end() || stored->second.enumerable;
  const bool configurable = stored == object.properties.end() || stored->second.configurable;
  const bool accessor = has_accessor(object, key);
  if (accessor) {
    const auto getter = object.getters.find(key);
    const auto setter = object.setters.find(key);
    const auto receiver_getter = object.receiver_getters.find(key);
    const auto receiver_setter = object.receiver_setters.find(key);
    // Accessors are stored as C++ callbacks, so the get/set fields are wrappers
    // rather than the original function objects (see ADR 0025).
    Value exposed_getter = Value::undefined();
    if (getter != object.getters.end()) {
      exposed_getter = native_value("get", [callback = getter->second](const std::vector<Value>&, Value) {
        return callback();
      });
    } else if (receiver_getter != object.receiver_getters.end()) {
      exposed_getter = native_value("get", [callback = receiver_getter->second](const std::vector<Value>&,
                                                                              Value self) {
        return callback(self);
      });
    }
    Value exposed_setter = Value::undefined();
    if (setter != object.setters.end()) {
      exposed_setter = native_value("set", [callback = setter->second](const std::vector<Value>& arguments, Value) {
        callback(arguments.empty() ? Value::undefined() : arguments[0]);
        return Value::undefined();
      });
    } else if (receiver_setter != object.receiver_setters.end()) {
      exposed_setter = native_value("set", [callback = receiver_setter->second](const std::vector<Value>& arguments,
                                                                              Value self) {
        callback(self, arguments.empty() ? Value::undefined() : arguments[0]);
        return Value::undefined();
      });
    }
    put("get", exposed_getter, true);
    put("set", exposed_setter, true);
  } else {
    Value value = Value::undefined();
    if (stored != object.properties.end()) {
      value = stored->second.value;
    } else {
      std::size_t index = 0;
      if (object.is_array() && is_array_index(key, index) && index < object.elements.size()) {
        value = object.elements[index];
      }
    }
    put("value", value, true);
    put("writable", Value::boolean_of(stored == object.properties.end() || stored->second.writable), true);
  }
  put("enumerable", Value::boolean_of(enumerable), true);
  put("configurable", Value::boolean_of(configurable), true);
  return Value::object_of(std::move(descriptor));
}

/// Own enumerable keys, as an array (`Object.keys`/`values`/`entries`).
Value own_keys_array(const Object& object) {
  std::vector<Value> keys;
  for (const std::string& key : own_property_keys(object, true)) keys.push_back(Value::string_of(key));
  return Value::object_of(make_array(std::move(keys)));
}

/// Reads a descriptor object (`{value: 1, writable: false}`) into the shape
/// [[DefineOwnProperty]] wants.
struct DescriptorInput {
  std::optional<Value> value;
  std::optional<Value> getter;
  std::optional<Value> setter;
  std::optional<bool> writable;
  std::optional<bool> enumerable;
  std::optional<bool> configurable;
};

void read_descriptor(const Value& descriptor, DescriptorInput& out) {
  if (!descriptor.is_object() || descriptor.object == nullptr) {
    throw thrown(type_error("property descriptor must be an object"));
  }
  auto read = [&](const std::string& name) -> std::optional<Value> {
    const auto found = descriptor.object->properties.find(name);
    if (found == descriptor.object->properties.end()) return std::nullopt;
    return found->second.value;
  };
  out.value = read("value");
  out.getter = read("get");
  out.setter = read("set");
  if (const auto attribute = read("writable")) out.writable = to_boolean(*attribute);
  if (const auto attribute = read("enumerable")) out.enumerable = to_boolean(*attribute);
  if (const auto attribute = read("configurable")) out.configurable = to_boolean(*attribute);
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
      if (!function->prototype_object) {
        function->prototype_object = std::make_shared<Object>();
        function->prototype_object->owner_function = function;
      }
      return Value::object_of(function->prototype_object);
    }
    if (function->statics != nullptr) {
      Value found;
      if (lookup_property(function->statics, key, found)) return found;
    }
    Value inherited;
    if (runtime_.object_prototype != nullptr && lookup_property(runtime_.object_prototype, key, inherited)) {
      return inherited;
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
    if (is_array_index(key, index) && index < target->elements.size()) return target->elements[index];
  }

  Value found;
  if (lookup_property(target, key, found)) return found;
  if (!target->prototype_is_null && runtime_.object_prototype != nullptr && target != runtime_.object_prototype &&
      lookup_property(runtime_.object_prototype, key, found)) {
    return found;
  }
  return Value::undefined();
}

void Interpreter::set_member(const Value& object, const std::string& key, const Value& value) {
  if (object.type == ValueType::Function && object.object == nullptr) {
    const std::shared_ptr<Object> bag = property_bag(object);
    const auto existing = bag->properties.find(key);
    if (existing != bag->properties.end()) {
      if (existing->second.writable) existing->second.value = value;
      return;
    }
    if (!bag->extensible) return;
    bag->properties[key] = Property{value};
    return;
  }
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
  for (std::shared_ptr<Object> ancestor = target; ancestor != nullptr; ancestor = ancestor->prototype) {
    const auto receiver_setter = ancestor->receiver_setters.find(key);
    if (receiver_setter != ancestor->receiver_setters.end()) {
      receiver_setter->second(Value::object_of(target), value);
      return;
    }
  }
  for (std::shared_ptr<Object> ancestor = target; ancestor != nullptr; ancestor = ancestor->prototype) {
    const auto inherited_setter = ancestor->setters.find(key);
    if (inherited_setter != ancestor->setters.end()) {
      inherited_setter->second(value);
      return;
    }
  }
  const auto existing = target->properties.find(key);
  if (existing != target->properties.end()) {
    if (!existing->second.writable) {
      // Sloppy mode ignores the write; strict mode throws (tracked in ADR 0025).
      return;
    }
    existing->second.value = value;
    return;
  }
  if (!target->extensible) return;
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
    function->prototype_object->owner_function = function;
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
            for (const std::string& key : own_property_keys(*iterable.object, true)) {
              items.push_back(Value::string_of(key));
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
        const Value value = eval(property.value, scope);
        if (property.kind == ast::PropertyDef::Kind::Data) {
          object->properties[property.key] = Property{value};
          continue;
        }
        if (property.kind == ast::PropertyDef::Kind::Get) {
          object->receiver_getters[property.key] = [value](const Value& self) {
            return call_function(value, self, {});
          };
        } else {
          object->receiver_setters[property.key] = [value](const Value& self, const Value& argument) {
            call_function(value, self, {argument});
          };
        }
        object->properties[property.key] = Property{Value::undefined(), true, true, true};
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
          const std::shared_ptr<Object> bag = property_bag(object);
          if (bag != nullptr) {
            const auto found = bag->properties.find(key);
            // `delete` only removes configurable properties (sloppy mode reports
            // false rather than throwing — strict mode is ADR 0025's open item).
            if (found != bag->properties.end() && !found->second.configurable) return Value::boolean_of(false);
            bag->properties.erase(key);
            bag->getters.erase(key);
            bag->setters.erase(key);
            bag->receiver_getters.erase(key);
            bag->receiver_setters.erase(key);
            std::size_t index = 0;
            if (bag->is_array() && is_array_index(key, index)) {
              if (index < bag->elements.size()) bag->elements[index] = Value::undefined();
            }
          }
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
  const Value array_constructor = native_value("Array", [](const std::vector<Value>& arguments, Value) {
    if (arguments.size() == 1 && arguments[0].is_number()) {
      return Value::object_of(make_array(std::vector<Value>(static_cast<std::size_t>(to_number(arguments[0])))));
    }
    return Value::object_of(make_array(arguments));
  });
  array_constructor.function->prototype_object = std::make_shared<Object>();
  array_constructor.function->prototype_object->owner_function = array_constructor.function;
  array_constructor.function->statics = std::make_shared<Object>();
  array_constructor.function->statics->properties["isArray"] = Property{native_value(
      "isArray", [](const std::vector<Value>& arguments, Value) {
        if (arguments.empty()) return Value::boolean_of(false);
        return Value::boolean_of(arguments[0].is_object() && arguments[0].object != nullptr &&
                                 arguments[0].object->is_array());
      })};
  global->variables["Array"] = array_constructor;

  // Object.prototype: the one object every plain object inherits from, so
  // `({}).hasOwnProperty('x')` and `Object.prototype.hasOwnProperty.call(…)` work.
  runtime.object_prototype = std::make_shared<Object>();
  const std::shared_ptr<Object> object_prototype = runtime.object_prototype;
  auto add_proto_method = [&](const char* name, Function::Native native) {
    object_prototype->properties[name] = Property{native_value(name, std::move(native))};
  };
  add_proto_method("hasOwnProperty", [](const std::vector<Value>& arguments, Value this_value) {
    const std::shared_ptr<Object> bag = property_bag(this_value);
    if (bag == nullptr) {
      throw thrown(type_error("hasOwnProperty called on " + typeof_string(this_value)));
    }
    const std::string key = arguments.empty() ? std::string() : to_string(arguments[0]);
    return Value::boolean_of(has_own_property(*bag, key));
  });
  add_proto_method("propertyIsEnumerable", [](const std::vector<Value>& arguments, Value this_value) {
    const std::shared_ptr<Object> bag = property_bag(this_value);
    if (bag == nullptr) {
      throw thrown(type_error("propertyIsEnumerable called on " + typeof_string(this_value)));
    }
    const std::string key = arguments.empty() ? std::string() : to_string(arguments[0]);
    if (!has_own_property(*bag, key)) return Value::boolean_of(false);
    const auto found = bag->properties.find(key);
    return Value::boolean_of(found == bag->properties.end() || found->second.enumerable);
  });
  add_proto_method("isPrototypeOf", [](const std::vector<Value>& arguments, Value this_value) {
    const std::shared_ptr<Object> bag = property_bag(this_value);
    if (bag == nullptr) return Value::boolean_of(false);
    const std::shared_ptr<Object> other = property_bag(arguments.empty() ? Value::undefined() : arguments[0]);
    if (other == nullptr) return Value::boolean_of(false);
    for (std::shared_ptr<Object> ancestor = other->prototype; ancestor != nullptr; ancestor = ancestor->prototype) {
      if (ancestor == bag) return Value::boolean_of(true);
    }
    return Value::boolean_of(false);
  });
  add_proto_method("toString", [](const std::vector<Value>&, Value this_value) {
    if (this_value.is_undefined()) return Value::string_of("[object Undefined]");
    if (this_value.is_null()) return Value::string_of("[object Null]");
    std::string tag = "Object";
    switch (this_value.type) {
      case ValueType::Boolean: tag = "Boolean"; break;
      case ValueType::Number: tag = "Number"; break;
      case ValueType::String: tag = "String"; break;
      case ValueType::Function: tag = "Function"; break;
      default: break;
    }
    if (this_value.type == ValueType::Function) {
      tag = "Function";
    } else if (this_value.is_object() && this_value.object != nullptr) {
      if (this_value.object->is_array()) {
        tag = "Array";
      } else if (this_value.object->kind == Object::Kind::Error) {
        tag = "Error";
      }
    }
    return Value::string_of("[object " + tag + "]");
  });
  add_proto_method("valueOf", [](const std::vector<Value>&, Value this_value) { return this_value; });
  add_proto_method("toLocaleString", [](const std::vector<Value>&, Value this_value) {
    return Value::string_of(to_string(this_value));
  });

  const Value object_constructor = native_value("Object", [](const std::vector<Value>& arguments, Value) {
    if (!arguments.empty() && arguments[0].is_object()) return arguments[0];
    if (!arguments.empty() && arguments[0].is_nullish()) return Value::object_of(std::make_shared<Object>());
    if (!arguments.empty()) {
      const Value primitives = arguments[0];
      auto wrapper = std::make_shared<Object>();
      wrapper->properties["value"] = Property{primitives};
      return Value::object_of(std::move(wrapper));
    }
    return Value::object_of(std::make_shared<Object>());
  });
  object_constructor.function->prototype_object = object_prototype;
  object_prototype->owner_function = object_constructor.function;
  object_constructor.function->statics = std::make_shared<Object>();
  auto add_object_static = [&](const char* name, Function::Native native) {
    object_constructor.function->statics->properties[name] = Property{native_value(name, std::move(native))};
  };
  // Object statics take an argument that is coerced with ToObject; primitives
  // have no own properties in this engine except string indices.
  auto object_argument = [](const std::vector<Value>& arguments, const std::string& method) {
    if (arguments.empty() || arguments[0].is_nullish()) {
      throw thrown(type_error("Object." + method + " called on " + (arguments.empty() ? "undefined" : to_string(arguments[0]))));
    }
    return arguments[0];
  };
  auto object_bag = [](const Value& value, const std::string& method) {
    const std::shared_ptr<Object> bag = property_bag(value);
    if (bag == nullptr) throw thrown(type_error("Object." + method + " called on non-object"));
    return bag;
  };
  add_object_static("keys", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "keys");
    if (target.type == ValueType::String) {
      std::vector<Value> keys;
      for (std::size_t i = 0; i < target.text.size(); ++i) keys.push_back(Value::string_of(std::to_string(i)));
      return Value::object_of(make_array(std::move(keys)));
    }
    const std::shared_ptr<Object> bag = property_bag(target);
    if (bag == nullptr) return Value::object_of(make_array({}));
    if (bag->is_array()) {
      std::vector<Value> keys;
      for (const std::string& key : own_property_keys(*bag, true)) keys.push_back(Value::string_of(key));
      return Value::object_of(make_array(std::move(keys)));
    }
    return own_keys_array(*bag);
  });
  add_object_static("values", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "values");
    std::vector<Value> values;
    const std::shared_ptr<Object> bag = property_bag(target);
    if (bag != nullptr) {
      for (const std::string& key : own_property_keys(*bag, true)) {
        Value value;
        if (lookup_property(bag, key, value)) values.push_back(value);
      }
    }
    return Value::object_of(make_array(std::move(values)));
  });
  add_object_static("entries", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "entries");
    std::vector<Value> entries;
    const std::shared_ptr<Object> bag = property_bag(target);
    if (bag != nullptr) {
      for (const std::string& key : own_property_keys(*bag, true)) {
        Value value;
        if (!lookup_property(bag, key, value)) continue;
        entries.push_back(Value::object_of(make_array({Value::string_of(key), value})));
      }
    }
    return Value::object_of(make_array(std::move(entries)));
  });
  add_object_static("getOwnPropertyNames", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "getOwnPropertyNames");
    std::vector<Value> keys;
    const std::shared_ptr<Object> bag = property_bag(target);
    if (bag != nullptr) {
      for (const std::string& key : own_property_keys(*bag, false)) keys.push_back(Value::string_of(key));
      if (bag->is_array()) keys.push_back(Value::string_of("length"));
    }
    return Value::object_of(make_array(std::move(keys)));
  });
  add_object_static("getOwnPropertyDescriptor", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "getOwnPropertyDescriptor");
    if (arguments.size() < 2) return Value::undefined();
    const std::string key = to_string(arguments[1]);
    const std::shared_ptr<Object> bag = property_bag(target);
    if (bag == nullptr) return Value::undefined();
    return property_descriptor(*bag, key);
  });
  add_object_static("defineProperty", [object_argument, object_bag](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "defineProperty");
    const std::shared_ptr<Object> bag = object_bag(target, "defineProperty");
    if (arguments.size() < 2) throw thrown(type_error("Object.defineProperty needs a property key"));
    DescriptorInput input;
    if (arguments.size() > 2) read_descriptor(arguments[2], input);
    define_own_property(bag, to_string(arguments[1]), input.value, input.getter, input.setter,
                        input.writable, input.enumerable, input.configurable);
    return target;
  });
  add_object_static("defineProperties", [object_argument, object_bag](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "defineProperties");
    const std::shared_ptr<Object> bag = object_bag(target, "defineProperties");
    if (arguments.size() > 1 && arguments[1].is_object() && arguments[1].object != nullptr) {
      for (const std::string& key : own_property_keys(*arguments[1].object, true)) {
        Value descriptor;
        if (!lookup_property(arguments[1].object, key, descriptor)) continue;
        DescriptorInput input;
        read_descriptor(descriptor, input);
        define_own_property(bag, key, input.value, input.getter, input.setter, input.writable, input.enumerable,
                            input.configurable);
      }
    }
    return target;
  });
  add_object_static("create", [object_prototype](const std::vector<Value>& arguments, Value) {
    auto created = std::make_shared<Object>();
    if (!arguments.empty() && arguments[0].is_null()) {
      created->prototype = nullptr;
      created->prototype_is_null = true;
    } else if (!arguments.empty() && arguments[0].is_object()) {
      created->prototype = arguments[0].object;
    } else if (!arguments.empty()) {
      throw thrown(type_error("Object prototype may only be an Object or null"));
    } else {
      created->prototype = object_prototype;
    }
    if (arguments.size() > 1 && arguments[1].is_object() && arguments[1].object != nullptr) {
      for (const std::string& key : own_property_keys(*arguments[1].object, true)) {
        Value descriptor;
        if (!lookup_property(arguments[1].object, key, descriptor)) continue;
        DescriptorInput input;
        read_descriptor(descriptor, input);
        define_own_property(created, key, input.value, input.getter, input.setter, input.writable, input.enumerable,
                            input.configurable);
      }
    }
    return Value::object_of(std::move(created));
  });
  add_object_static("getPrototypeOf", [object_argument, object_prototype](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "getPrototypeOf");
    if (target.type != ValueType::Object || target.object == nullptr) {
      return Value::object_of(object_prototype);
    }
    if (target.object->prototype == nullptr) {
      // Plain objects have no explicit link; they inherit from Object.prototype.
      if (target.object == object_prototype) return Value::null();
      return Value::object_of(object_prototype);
    }
    return Value::object_of(target.object->prototype);
  });
  add_object_static("setPrototypeOf", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "setPrototypeOf");
    if (target.type != ValueType::Object || target.object == nullptr) return target;
    if (arguments.size() > 1 && arguments[1].is_object()) {
      target.object->prototype = arguments[1].object;
      target.object->prototype_is_null = false;
    } else if (arguments.size() > 1 && arguments[1].is_null()) {
      target.object->prototype = nullptr;
      target.object->prototype_is_null = true;
    } else if (arguments.size() > 1) {
      target.object->prototype_is_null = false;
    }
    return target;
  });
  add_object_static("freeze", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "freeze");
    if (target.type != ValueType::Object || target.object == nullptr) return target;
    for (const std::string& key : own_property_keys(*target.object, false)) {
      auto found = target.object->properties.find(key);
      if (found == target.object->properties.end()) {
        target.object->properties[key] = Property{Value::undefined(), false, true, false};
      } else {
        found->second.writable = false;
        found->second.configurable = false;
      }
    }
    target.object->extensible = false;
    return target;
  });
  add_object_static("seal", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "seal");
    if (target.type != ValueType::Object || target.object == nullptr) return target;
    for (const std::string& key : own_property_keys(*target.object, false)) {
      auto found = target.object->properties.find(key);
      if (found != target.object->properties.end()) found->second.configurable = false;
    }
    target.object->extensible = false;
    return target;
  });
  add_object_static("preventExtensions", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "preventExtensions");
    if (target.type == ValueType::Object && target.object != nullptr) target.object->extensible = false;
    return target;
  });
  add_object_static("isExtensible", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "isExtensible");
    if (target.type != ValueType::Object || target.object == nullptr) return Value::boolean_of(false);
    return Value::boolean_of(target.object->extensible);
  });
  add_object_static("isFrozen", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "isFrozen");
    if (target.type != ValueType::Object || target.object == nullptr) return Value::boolean_of(true);
    if (target.object->extensible) return Value::boolean_of(false);
    for (const std::string& key : own_property_keys(*target.object, false)) {
      const auto found = target.object->properties.find(key);
      if (found == target.object->properties.end()) continue;
      if (found->second.configurable) return Value::boolean_of(false);
      if (found->second.writable && !has_accessor(*target.object, key)) return Value::boolean_of(false);
    }
    return Value::boolean_of(true);
  });
  add_object_static("isSealed", [object_argument](const std::vector<Value>& arguments, Value) {
    const Value target = object_argument(arguments, "isSealed");
    if (target.type != ValueType::Object || target.object == nullptr) return Value::boolean_of(true);
    if (target.object->extensible) return Value::boolean_of(false);
    for (const std::string& key : own_property_keys(*target.object, false)) {
      const auto found = target.object->properties.find(key);
      if (found != target.object->properties.end() && found->second.configurable) return Value::boolean_of(false);
    }
    return Value::boolean_of(true);
  });
  add_object_static("is", [](const std::vector<Value>& arguments, Value) {
    const Value first = arguments.empty() ? Value::undefined() : arguments[0];
    const Value second = arguments.size() < 2 ? Value::undefined() : arguments[1];
    if (first.is_number() && second.is_number()) {
      const double a = first.number;
      const double b = second.number;
      if (std::isnan(a) && std::isnan(b)) return Value::boolean_of(true);
      if (a == 0.0 && b == 0.0) return Value::boolean_of(std::signbit(a) == std::signbit(b));
    }
    return Value::boolean_of(strict_equals(first, second));
  });
  add_object_static("assign", [](const std::vector<Value>& arguments, Value) {
    if (arguments.empty() || !arguments[0].is_object() || arguments[0].object == nullptr) {
      throw thrown(type_error("Object.assign target must be an object"));
    }
    const std::shared_ptr<Object> target = arguments[0].object;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
      if (!arguments[i].is_object() || arguments[i].object == nullptr) continue;
      const std::shared_ptr<Object> source = arguments[i].object;
      for (const std::string& key : own_property_keys(*source, true)) {
        Value value;
        if (!lookup_property(source, key, value)) continue;
        target->properties[key] = Property{value};
      }
    }
    return arguments[0];
  });
  global->variables["Object"] = object_constructor;

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
