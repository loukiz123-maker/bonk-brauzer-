#include "bonk/core/selftest.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "bonk/core/arena.hpp"
#include "bonk/core/diag.hpp"
#include "bonk/core/module.hpp"
#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"

namespace bonk {
namespace {

class Recorder {
 public:
  explicit Recorder(SelfTestReport& report) : report_(report) {}

  void check(bool condition, std::string_view name, std::string_view detail = {}) {
    std::string line(name);
    if (condition) {
      line += ": ok";
      report_.checks.push_back(std::move(line));
    } else {
      line += ": FAILED";
      if (!detail.empty()) {
        line += " (";
        line += detail;
        line += ")";
      }
      report_.failures.push_back(std::move(line));
    }
  }

  void expect_eq(std::string_view name, const std::string& actual, const std::string& expected) {
    check(actual == expected, name, "expected <" + expected + "> got <" + actual + ">");
  }

 private:
  SelfTestReport& report_;
};

void check_utf8(Recorder& rec) {
  const char32_t samples[] = {U'A', 0x7F, 0x80, 0x7FF, 0x800, 0xFFFD, 0xFFFF, 0x10000, 0x10FFFF, U'я'};
  bool roundtrip_ok = true;
  for (const char32_t code_point : samples) {
    std::string encoded;
    append_utf8(encoded, code_point);
    const DecodedCodePoint decoded = decode_utf8(encoded);
    if (!decoded.valid || decoded.code_point != code_point || decoded.length != encoded.size()) {
      roundtrip_ok = false;
      break;
    }
  }
  rec.check(roundtrip_ok, "utf8.roundtrip");

  const std::string invalid = std::string("\xFF", 1);
  const DecodedCodePoint bad = decode_utf8(invalid);
  rec.check(!bad.valid && bad.length == 1 && bad.code_point == kReplacementCharacter, "utf8.invalid_byte");

  // ED A0 80 is the UTF-8 encoding of a lone surrogate: rejected per WHATWG.
  const std::string surrogate("\xED\xA0\x80", 3);
  rec.check(!is_valid_utf8(surrogate), "utf8.reject_surrogate");
  rec.check(is_valid_utf8("привет, мир"), "utf8.valid_multibyte");
  rec.check(utf8_length("привет") == 6, "utf8.length_cyrillic");
}

void check_strings(Recorder& rec) {
  rec.check(trim("  \t hi \n") == "hi", "strings.trim");
  rec.check(equals_ci("SECRET", "secret"), "strings.equals_ci");
  const std::vector<std::string_view> parts = split("a,,b,", ',');
  rec.check(parts.size() == 4 && parts[1].empty() && parts[3].empty(), "strings.split_empty_fields");
  unsigned long long value = 0;
  rec.check(parse_u64("42", value) && value == 42, "strings.parse_u64");
  unsigned long long ignored = 0;
  rec.check(!parse_u64("4x", ignored), "strings.parse_u64_rejects");
}

void check_arena(Recorder& rec) {
  Arena arena(512);
  bool aligned = true;
  for (std::size_t alignment = 1; alignment <= 64; alignment *= 2) {
    for (std::size_t size : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{100}}) {
      void* pointer = arena.allocate(size, alignment);
      if (reinterpret_cast<std::uintptr_t>(pointer) % alignment != 0) aligned = false;
    }
  }
  rec.check(aligned, "arena.alignment");
  rec.check(arena.bytes_used() > 0, "arena.counts_usage");

  const std::size_t before = arena.blocks();
  arena.reset();
  rec.check(arena.bytes_used() == 0 && before > 1, "arena.reset_reclaims");
}

void check_diagnostics(Recorder& rec) {
  Diagnostics diagnostics;
  diagnostics.error(SourcePos::at_line("index.html", 12, 5), "unclosed <div>");
  diagnostics.warn(SourcePos::at(4096), "duplicate attribute");
  rec.check(diagnostics.count(Severity::Error) == 1 && diagnostics.has_errors(), "diag.counts");

  const std::string first = format_diagnostic(diagnostics.items()[0]);
  rec.expect_eq("diag.format_with_position", first, "index.html:12:5: error: unclosed <div>");
  const std::string second = format_diagnostic(diagnostics.items()[1]);
  rec.expect_eq("diag.format_offset_only", second, "warning: duplicate attribute");
}

bool dependencies_form_a_dag() {
  // Kahn's algorithm over the module ids: any leftover node means a cycle.
  const std::vector<ModuleInfo>& all = modules();
  std::vector<std::size_t> indegree(all.size(), 0);
  std::vector<std::vector<std::size_t>> edges(all.size());

  for (std::size_t i = 0; i < all.size(); ++i) {
    for (const std::string_view dependency : split(all[i].depends_on, ',')) {
      const std::string_view id = trim(dependency);
      if (id.empty()) continue;
      for (std::size_t j = 0; j < all.size(); ++j) {
        if (all[j].id == id) {
          edges[j].push_back(i);
          ++indegree[i];
          break;
        }
      }
    }
  }

  std::vector<std::size_t> queue;
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (indegree[i] == 0) queue.push_back(i);
  }
  std::size_t visited = 0;
  while (!queue.empty()) {
    const std::size_t node = queue.back();
    queue.pop_back();
    ++visited;
    for (const std::size_t next : edges[node]) {
      if (--indegree[next] == 0) queue.push_back(next);
    }
  }
  return visited == all.size();
}

void check_modules(Recorder& rec) {
  const std::vector<ModuleInfo>& all = modules();
  rec.check(all.size() == 13, "modules.count");

  std::set<std::string_view> ids;
  bool unique = true;
  bool stages_valid = true;
  bool deps_known = true;
  for (const ModuleInfo& module : all) {
    if (!ids.insert(module.id).second) unique = false;
    if (module.stage < 0 || module.stage > 7) stages_valid = false;
    for (const std::string_view dependency : split(module.depends_on, ',')) {
      const std::string_view id = trim(dependency);
      if (!id.empty() && find_module(id) == nullptr) deps_known = false;
    }
  }
  rec.check(unique, "modules.unique_ids");
  rec.check(stages_valid, "modules.stage_range");
  rec.check(deps_known, "modules.known_dependencies");
  rec.check(dependencies_form_a_dag(), "modules.acyclic");
  rec.check(find_module("core") != nullptr && find_module("nope") == nullptr, "modules.lookup");
  rec.check(!render_module_table(0).empty(), "modules.render_table");
}

}  // namespace

SelfTestReport run_self_test() {
  SelfTestReport report;
  Recorder recorder(report);
  check_utf8(recorder);
  check_strings(recorder);
  check_arena(recorder);
  check_diagnostics(recorder);
  check_modules(recorder);
  return report;
}

}  // namespace bonk
