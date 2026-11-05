// html5lib tokenizer corpus runner.
//
// Corpus: https://github.com/html5lib/html5lib-tests — `tokenizer/*.test`, a set of
// JSON files where each case is `{input, output, initialStates?, lastStartTag?,
// doubleEscaped?}` and `output` is a list of tokens in the html5lib notation
// (Character runs, StartTag/EndTag with an attribute object, Comment, DOCTYPE
// with four or five fields, and ParseError markers which we ignore).
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/conformance/conformance.hpp"
#include "bonk/html/tokenizer.hpp"
#include "json.hpp"

namespace bonk::conformance {
namespace {

using json::Value;

// Expectations that predate the current WHATWG text and that we deliberately do
// not follow. Each entry is (file, description substring, reason) and is counted
// as "skipped", never as a pass — the reason is printed in the report.
struct KnownDivergence {
  const char* file;
  const char* description;  // matched as a prefix (may contain NUL escapes)
  const char* reason;
};

constexpr KnownDivergence kKnownDivergences[] = {
    {"test2.test", "Null Byte Replacement", "legacy: expects U+0000 in data; the spec emits U+FFFD"},
    {"test3.test", "<\\u0000", "legacy: expects U+0000 in data; the spec emits U+FFFD"},
    {"test3.test", "<?", "legacy: expects no comment token for '<?'; the spec emits a bogus comment"},
    {"test4.test", "U+0000 in lookahead region", "legacy: expects U+0000 in data; the spec emits U+FFFD"},
    {"unicodeCharsProblematic.test", "CR followed by U+0000", "legacy: expects U+0000 in data; the spec emits U+FFFD"},
};

[[nodiscard]] const KnownDivergence* divergence(const std::string& file, const std::string& description) {
  for (const KnownDivergence& entry : kKnownDivergences) {
    if (file == entry.file && description.compare(0, std::strlen(entry.description), entry.description) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

struct ExpectedToken {
  enum class Kind { Doctype, StartTag, EndTag, Comment, Characters } kind = Kind::Characters;
  std::string name;
  std::string data;
  std::vector<std::pair<std::string, std::string>> attributes;
  bool self_closing = false;
  // DOCTYPE only: `has_ids` is true when the token carried a public/system
  // identifier at all (html5lib writes null for "absent", "" for "empty").
  bool has_ids = false;
  std::string public_id;
  std::string system_id;
  bool force_quirks = false;
};

[[nodiscard]] std::string printable(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (byte < 0x20 || byte == 0x7F) {
      const char* digits = "0123456789abcdef";
      out += "\\x";
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0xF]);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

[[nodiscard]] std::string describe(const ExpectedToken& token) {
  switch (token.kind) {
    case ExpectedToken::Kind::Doctype: {
      std::string out = "DOCTYPE(" + printable(token.name);
      if (token.has_ids) out += " \"" + printable(token.public_id) + "\" \"" + printable(token.system_id) + "\"";
      if (token.force_quirks) out += " !quirks";
      return out + ")";
    }
    case ExpectedToken::Kind::Comment: return "Comment(" + printable(token.data) + ")";
    case ExpectedToken::Kind::Characters: return "Character(" + printable(token.data) + ")";
    case ExpectedToken::Kind::StartTag:
    case ExpectedToken::Kind::EndTag: {
      std::string out = token.kind == ExpectedToken::Kind::StartTag ? "StartTag(" : "EndTag(";
      out += printable(token.name);
      for (const auto& [name, value] : token.attributes) {
        out += " " + printable(name) + "=" + printable(value);
      }
      if (token.self_closing) out += " /";
      out += ")";
      return out;
    }
  }
  return "?";
}

void merge_character_runs(std::vector<ExpectedToken>& tokens) {
  std::vector<ExpectedToken> merged;
  for (auto& token : tokens) {
    if (token.kind == ExpectedToken::Kind::Characters && !merged.empty() &&
        merged.back().kind == ExpectedToken::Kind::Characters) {
      merged.back().data += token.data;
    } else {
      merged.push_back(std::move(token));
    }
  }
  tokens = std::move(merged);
}

/// Converts one html5lib output entry into our shape; returns false for entries
/// we cannot model (foreign-content namespace prefixes) or ParseError markers.
enum class EntryStatus { Ok, Ignore, Unsupported };

[[nodiscard]] EntryStatus from_expected(const Value& entry, bool double_escaped, ExpectedToken& out) {
  if (entry.kind != Value::Kind::Array || entry.items.empty()) return EntryStatus::Unsupported;
  const std::string kind = entry.items[0].string_or_empty();
  auto decode = [double_escaped](const std::string& text) {
    return double_escaped ? json::unescape(text) : text;
  };
  if (kind == "ParseError") return EntryStatus::Ignore;
  if (kind == "DOCTYPE") {
    out.kind = ExpectedToken::Kind::Doctype;
    out.name = entry.items.size() > 1 ? decode(entry.items[1].string_or_empty()) : std::string{};
    const bool has_public = entry.items.size() > 2 && !entry.items[2].is_null();
    const bool has_system = entry.items.size() > 3 && !entry.items[3].is_null();
    out.has_ids = has_public || has_system;
    if (has_public) out.public_id = decode(entry.items[2].string_or_empty());
    if (has_system) out.system_id = decode(entry.items[3].string_or_empty());
    // The fifth field is html5lib's "correctness": false means the tokenizer set
    // its force-quirks flag.
    out.force_quirks = entry.items.size() > 4 && entry.items[4].kind == Value::Kind::Bool && !entry.items[4].boolean;
    return EntryStatus::Ok;
  }
  if (kind == "Comment") {
    out.kind = ExpectedToken::Kind::Comment;
    out.data = entry.items.size() > 1 ? decode(entry.items[1].string_or_empty()) : std::string{};
    return EntryStatus::Ok;
  }
  if (kind == "Character") {
    out.kind = ExpectedToken::Kind::Characters;
    out.data = entry.items.size() > 1 ? decode(entry.items[1].string_or_empty()) : std::string{};
    return EntryStatus::Ok;
  }
  if (kind == "StartTag" || kind == "EndTag") {
    out.kind = kind == "StartTag" ? ExpectedToken::Kind::StartTag : ExpectedToken::Kind::EndTag;
    out.name = entry.items.size() > 1 ? entry.items[1].string_or_empty() : std::string{};
    if (out.name.find(':') != std::string::npos) return EntryStatus::Unsupported;
    if (entry.items.size() > 2 && entry.items[2].kind == Value::Kind::Object) {
      for (const auto& [name, value] : entry.items[2].members) {
        out.attributes.emplace_back(decode(name), value.is_null() ? std::string{} : decode(value.string_or_empty()));
      }
      std::sort(out.attributes.begin(), out.attributes.end());
    }
    out.self_closing = entry.items.size() > 3 && entry.items[3].kind == Value::Kind::Bool && entry.items[3].boolean;
    return EntryStatus::Ok;
  }
  return EntryStatus::Unsupported;
}

[[nodiscard]] ExpectedToken from_actual(const html::Token& token) {
  ExpectedToken out;
  switch (token.type) {
    case html::TokenType::Doctype:
      out.kind = ExpectedToken::Kind::Doctype;
      out.name = token.data;
      out.has_ids = token.has_public_identifier || token.has_system_identifier;
      out.public_id = token.public_identifier;
      out.system_id = token.system_identifier;
      out.force_quirks = token.force_quirks;
      break;
    case html::TokenType::Comment:
      out.kind = ExpectedToken::Kind::Comment;
      out.data = token.data;
      break;
    case html::TokenType::Characters:
      out.kind = ExpectedToken::Kind::Characters;
      out.data = token.data;
      break;
    case html::TokenType::StartTag:
    case html::TokenType::EndTag:
      out.kind = token.type == html::TokenType::StartTag ? ExpectedToken::Kind::StartTag : ExpectedToken::Kind::EndTag;
      out.name = token.name;
      out.attributes = token.attributes;
      std::sort(out.attributes.begin(), out.attributes.end());
      out.self_closing = token.self_closing;
      break;
    case html::TokenType::EndOfFile: break;
  }
  return out;
}

[[nodiscard]] bool same(const ExpectedToken& a, const ExpectedToken& b) {
  return a.kind == b.kind && a.name == b.name && a.data == b.data && a.attributes == b.attributes &&
         a.self_closing == b.self_closing && a.has_ids == b.has_ids && a.public_id == b.public_id &&
         a.system_id == b.system_id && a.force_quirks == b.force_quirks;
}

/// Runs one case; fills `detail` on mismatch.
[[nodiscard]] bool run_case(const std::string& input, const std::string& initial_state,
                            const std::string& last_start_tag, const std::vector<ExpectedToken>& expected,
                            std::string& detail) {
  html::Tokenizer tokenizer{input, nullptr};
  if (initial_state == "PLAINTEXT state") {
    tokenizer.enter_plain_text();
  } else if (initial_state == "RCDATA state") {
    tokenizer.enter_text_element(last_start_tag.empty() ? std::string_view{"title"} : std::string_view{last_start_tag},
                                 true);
  } else if (initial_state == "RAWTEXT state") {
    tokenizer.enter_text_element(last_start_tag.empty() ? std::string_view{"style"} : std::string_view{last_start_tag},
                                 false);
  } else if (initial_state == "Script data state") {
    tokenizer.enter_text_element(last_start_tag.empty() ? std::string_view{"script"} : std::string_view{last_start_tag},
                                 false);
  }

  std::vector<ExpectedToken> actual;
  for (std::size_t guard = 0; guard < input.size() * 4 + 64; ++guard) {
    const html::Token token = tokenizer.next();
    if (token.type == html::TokenType::EndOfFile) break;
    actual.push_back(from_actual(token));
  }
  merge_character_runs(actual);

  const std::size_t common = std::min(actual.size(), expected.size());
  for (std::size_t i = 0; i < common; ++i) {
    if (same(actual[i], expected[i])) continue;
    detail = "token " + std::to_string(i) + ": expected " + describe(expected[i]) + ", got " + describe(actual[i]);
    return false;
  }
  if (actual.size() != expected.size()) {
    detail = "token count: expected " + std::to_string(expected.size()) + ", got " + std::to_string(actual.size());
    if (actual.size() > common) {
      detail += " (extra " + describe(actual[common]) + ")";
    } else {
      detail += " (missing " + describe(expected[common]) + ")";
    }
    return false;
  }
  return true;
}

}  // namespace

SuiteReport run_tokenizer_corpus(std::string_view directory, const SuiteOptions& options) {
  SuiteReport report;
  report.suite = "html5lib-tokenizer";
  report.source = std::string(directory);
  if (!detail::directory_exists(directory)) return report;

  std::size_t state_skips = 0;
  std::size_t namespace_skips = 0;
  std::size_t divergence_skips = 0;

  for (const std::string& path : detail::list_files(directory, ".test")) {
    const std::string text = detail::read_file(path);
    if (text.empty()) continue;
    Value root;
    if (!json::parse(text, root)) continue;
    const Value* tests = root.find("tests");
    if (tests == nullptr || tests->kind != Value::Kind::Array) continue;
    const std::string file = std::filesystem::path(path).filename().string();

    for (const Value& test : tests->items) {
      if (options.limit != 0 && report.total >= options.limit) {
        report.notes = {"skipped (unknown initial state): " + std::to_string(state_skips),
                        "skipped (foreign-content names): " + std::to_string(namespace_skips),
                        "skipped (documented divergence): " + std::to_string(divergence_skips)};
        return report;
      }
      ++report.total;
      const std::string description = test.find("description") ? test.find("description")->string_or_empty() : file;
      if (divergence(file, description) != nullptr) {
        ++report.skipped;
        ++divergence_skips;
        continue;
      }
      const std::string input_raw = test.find("input") ? test.find("input")->string_or_empty() : std::string{};
      const bool double_escaped = test.find("doubleEscaped") != nullptr;
      const std::string input = double_escaped ? json::unescape(input_raw) : input_raw;
      const std::string last_start_tag =
          test.find("lastStartTag") ? test.find("lastStartTag")->string_or_empty() : std::string{};

      std::vector<std::string> initial_states{"Data state"};
      if (const Value* states = test.find("initialStates"); states != nullptr && states->kind == Value::Kind::Array) {
        initial_states.clear();
        for (const Value& state : states->items) initial_states.push_back(state.string_or_empty());
      }

      const Value* output = test.find("output");
      if (output == nullptr || output->kind != Value::Kind::Array) {
        ++report.skipped;
        continue;
      }
      std::vector<ExpectedToken> expected;
      bool unsupported = false;
      for (const Value& entry : output->items) {
        ExpectedToken token;
        const EntryStatus status = from_expected(entry, double_escaped, token);
        if (status == EntryStatus::Unsupported) {
          unsupported = true;
          ++namespace_skips;
          break;
        }
        if (status == EntryStatus::Ignore) continue;
        expected.push_back(std::move(token));
      }
      if (unsupported) {
        ++report.skipped;
        continue;
      }
      merge_character_runs(expected);

      bool ok = true;
      std::string detail;
      bool state_supported = true;
      for (const std::string& state : initial_states) {
        if (state != "Data state" && state != "RCDATA state" && state != "RAWTEXT state" &&
            state != "Script data state" && state != "PLAINTEXT state") {
          state_supported = false;  // e.g. "CDATA section state", which the tree builder owns
          break;
        }
      }
      if (!state_supported) {
        ++report.skipped;
        ++state_skips;
        continue;
      }
      for (const std::string& state : initial_states) {
        std::string state_detail;
        if (!run_case(input, state, last_start_tag, expected, state_detail)) {
          ok = false;
          detail = (initial_states.size() > 1 ? state + ": " : "") + state_detail;
          break;
        }
      }
      if (ok) {
        ++report.passed;
      } else if (report.failures.size() < options.max_failures) {
        report.failures.push_back({file + " / " + description, detail + " [input " + printable(input) + "]"});
      }
    }
  }
  report.notes = {"skipped (unknown initial state): " + std::to_string(state_skips),
                  "skipped (foreign-content names): " + std::to_string(namespace_skips),
                  "skipped (documented divergence): " + std::to_string(divergence_skips)};
  return report;
}

}  // namespace bonk::conformance
