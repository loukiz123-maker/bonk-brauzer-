// Minimal JSON reader used only by the conformance harness (the tokenizer corpus
// and test262's `*.json` manifests). Objects keep insertion order so attribute
// comparison can stay deterministic.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bonk::conformance::json {

struct Value {
  enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> members;

  [[nodiscard]] bool is_null() const noexcept { return kind == Kind::Null; }
  [[nodiscard]] const Value* find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto& [name, value] : members) {
      if (name == key) return &value;
    }
    return nullptr;
  }
  [[nodiscard]] std::string string_or_empty() const { return kind == Kind::String ? text : std::string{}; }
};

/// Returns false (and leaves `out` empty) on malformed input.
[[nodiscard]] bool parse(std::string_view text, Value& out);

/// Unescapes `\uXXXX`/`\n`/`\\` sequences once more; the tokenizer corpus marks
/// some cases `doubleEscaped`, meaning the JSON already decoded one layer.
[[nodiscard]] std::string unescape(std::string_view text);

}  // namespace bonk::conformance::json
