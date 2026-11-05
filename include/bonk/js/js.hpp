// Bonk Brauzer — a JavaScript engine written from scratch.
//
// No V8, no SpiderMonkey, no QuickJS: our own lexer, recursive-descent parser,
// AST and tree-walking interpreter. ES5-ish semantics plus the ES2015 pieces a
// page actually needs (let/const, arrow functions, template literals, classes
// are NOT implemented — see docs/STAGE-04.md).
//
// Memory model: values are shared_ptr-managed (std::shared_ptr is standard
// library, not an external engine). Cycles in user objects therefore leak; a
// tracing collector is planned for stage 6 and the limitation is documented.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk::js {

struct Object;
struct Function;

enum class ValueType { Undefined, Null, Boolean, Number, String, Object, Function };

struct Value {
  ValueType type = ValueType::Undefined;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::shared_ptr<Object> object;
  std::shared_ptr<Function> function;

  static Value undefined() { return Value{}; }
  static Value null() {
    Value value;
    value.type = ValueType::Null;
    return value;
  }
  static Value boolean_of(bool value_in) {
    Value value;
    value.type = ValueType::Boolean;
    value.boolean = value_in;
    return value;
  }
  static Value number_of(double value_in) {
    Value value;
    value.type = ValueType::Number;
    value.number = value_in;
    return value;
  }
  static Value string_of(std::string value_in) {
    Value value;
    value.type = ValueType::String;
    value.text = std::move(value_in);
    return value;
  }
  static Value object_of(std::shared_ptr<Object> value_in);
  static Value function_of(std::shared_ptr<Function> value_in);

  [[nodiscard]] bool is_undefined() const noexcept { return type == ValueType::Undefined; }
  [[nodiscard]] bool is_null() const noexcept { return type == ValueType::Null; }
  [[nodiscard]] bool is_nullish() const noexcept { return is_undefined() || is_null(); }
  [[nodiscard]] bool is_number() const noexcept { return type == ValueType::Number; }
  [[nodiscard]] bool is_string() const noexcept { return type == ValueType::String; }
  [[nodiscard]] bool is_boolean() const noexcept { return type == ValueType::Boolean; }
  [[nodiscard]] bool is_object() const noexcept { return type == ValueType::Object || type == ValueType::Function; }
};

/// A callable. Native functions close over C++ lambdas; user functions over an AST.
struct Function {
  using Native = std::function<Value(const std::vector<Value>& arguments, Value this_value)>;

  std::string name;
  Native native;                    // set for builtins
  std::shared_ptr<void> code;       // ast::Function for user code (type-erased to keep this header light)
  void* closure = nullptr;          // enclosing Scope, owned by the Runtime's scope arena
                                    // (non-owning: scopes outlive every function value, see ADR 0006)
  std::size_t parameter_count = 0;
  Value prototype_value;
  std::shared_ptr<Object> prototype_object;  // `new` puts instance.__proto__ here
  /// Static properties of a builtin constructor (`Object.keys`, `Array.isArray`):
  /// functions are plain Values, so they carry their own namespace object.
  std::shared_ptr<Object> statics;

  [[nodiscard]] bool is_native() const noexcept { return static_cast<bool>(native); }
};

struct Property {
  Value value;
  bool writable = true;
  bool enumerable = true;
  bool configurable = true;
};

/// A plain object / array / function object / error object.
struct Object {
  enum class Kind { Plain, Array, Function, Error };

  using Getter = std::function<Value()>;
  using Setter = std::function<void(const Value&)>;
  /// Accessors that need their receiver: `{get x() { return this.y; }}` and
  /// `Object.defineProperty(o, 'x', {get: …})` (the DOM bindings use the
  /// receiver-less Getter/Setter above, which need no `this`).
  using ReceiverGetter = std::function<Value(const Value&)>;
  using ReceiverSetter = std::function<void(const Value&, const Value&)>;

  Kind kind = Kind::Plain;
  std::unordered_map<std::string, Property> properties;
  std::vector<Value> elements;  // only for Kind::Array
  std::shared_ptr<Function> function;
  std::shared_ptr<Object> prototype;
  /// `Object.create(null)` / `setPrototypeOf(o, null)`: the chain really ends
  /// here, so the implicit Object.prototype fallback must not apply.
  bool prototype_is_null = false;
  /// `arguments`: only affects `Object.prototype.toString` ([object Arguments]).
  bool is_arguments_object = false;
  /// Set on a function's `prototype` object so `f.prototype.constructor === f`
  /// without a shared_ptr cycle (the prototype only *observes* the function).
  std::weak_ptr<Function> owner_function;
  /// `Object.preventExtensions`/`freeze`: a non-extensible object cannot grow.
  bool extensible = true;
  /// Accessor properties: how the bindings layer makes `el.textContent = 'x'`
  /// write through to the DOM without a special object kind.
  std::unordered_map<std::string, Getter> getters;
  std::unordered_map<std::string, Setter> setters;
  std::unordered_map<std::string, ReceiverGetter> receiver_getters;
  std::unordered_map<std::string, ReceiverSetter> receiver_setters;

  [[nodiscard]] bool is_array() const noexcept { return kind == Kind::Array; }
  [[nodiscard]] bool is_function() const noexcept { return kind == Kind::Function; }
};

inline Value Value::object_of(std::shared_ptr<Object> value_in) {
  Value value;
  value.type = ValueType::Object;
  value.object = std::move(value_in);
  return value;
}

inline Value Value::function_of(std::shared_ptr<Function> value_in) {
  Value value;
  value.type = ValueType::Function;
  value.function = std::move(value_in);
  return value;
}

/// Coercions, matching the parts of the spec a page notices.
[[nodiscard]] bool to_boolean(const Value& value);
[[nodiscard]] double to_number(const Value& value);
[[nodiscard]] std::string to_string(const Value& value);
[[nodiscard]] std::string typeof_string(const Value& value);
/// Abstract equality (==) and strict equality (===).
[[nodiscard]] bool loose_equals(const Value& a, const Value& b);
[[nodiscard]] bool strict_equals(const Value& a, const Value& b);
[[nodiscard]] std::string number_to_string(double value);

/// Formats a thrown value the way console output/errors do.
[[nodiscard]] std::string describe_exception(const Value& value);

struct ConsoleEntry {
  enum class Level { Log, Warn, Error } level = Level::Log;
  std::string message;
};

class Runtime;
class Scope;

/// Host-facing view of a context's runtime: global variables, native callbacks
/// and the console buffer. Used by the bindings layer and by the tests.
class Runtime {
 public:
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  /// Calls a JS function from C++ (host callbacks, event handlers).
  [[nodiscard]] Value call(const Value& callee, std::vector<Value> arguments);

  /// Reads a global variable (undefined when absent).
  [[nodiscard]] Value global(std::string_view name) const;
  void set_global(std::string name, Value value);

  void log(ConsoleEntry::Level level, std::string message);
  [[nodiscard]] const std::vector<ConsoleEntry>& console() const;
  [[nodiscard]] const std::unordered_map<std::string, std::function<Value(const std::vector<Value>&)>>&
  host_callbacks() const;

 private:
  friend class Context;
  Runtime();
  ~Runtime();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Result of executing a script: either a value or a thrown exception.
struct ExecutionResult {
  bool ok = true;
  Value value;
  Value exception;
  std::string message;
};

/// Compiles and runs `source`. Errors carry codes "js.syntax" / "js.runtime".
class Context {
 public:
  Context();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  /// Installs the global `document`/`window` hooks used by the bindings layer.
  void set_global(std::string name, Value value);

  [[nodiscard]] ExecutionResult evaluate(std::string_view source, std::string_view origin = "<script>");

  /// Runs `callback` with the current runtime (used by the bindings layer).
  void set_host_callback(std::string name, std::function<Value(const std::vector<Value>& arguments)> callback);

  [[nodiscard]] const std::vector<ConsoleEntry>& console() const noexcept;
  void clear_console();

  /// Runs pending setTimeout callbacks (0 ms timers fire immediately).
  void run_pending_callbacks();

  [[nodiscard]] Runtime& runtime();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bonk::js
