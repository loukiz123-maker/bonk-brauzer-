// Bonk Brauzer — module registry.
//
// This table is the single source of truth for "what exists, which stage it
// belongs to, what it depends on". It is used by:
//   * the CLI (`bonk modules`), so the roadmap is visible from the binary;
//   * the build/CI scripts, to decide which stage's exit criteria to check;
//   * docs/modules.generated.md, regenerated with `make docs`.
//
// Keeping it in code (not only in Markdown) means it cannot rot unnoticed:
// a unit test verifies that every dependency id exists and that the
// dependency graph is acyclic.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bonk {

enum class ModuleStatus { Planned, InProgress, Done };

[[nodiscard]] std::string_view to_string(ModuleStatus status) noexcept;

struct ModuleInfo {
  std::string_view id;       // "html"
  std::string_view title;    // "HTML parser"
  std::string_view summary;  // one line, shown by `bonk modules --long`
  int stage = 0;             // 0..7, the stage that delivers it
  ModuleStatus status = ModuleStatus::Planned;
  std::string_view depends_on;  // comma separated module ids, may be empty
};

/// All modules in dependency-friendly order (core first).
[[nodiscard]] const std::vector<ModuleInfo>& modules();

[[nodiscard]] const ModuleInfo* find_module(std::string_view id);

/// Modules whose owning stage is <= `stage`.
[[nodiscard]] std::vector<const ModuleInfo*> modules_available_at(int stage);

[[nodiscard]] std::string_view stage_title(int stage) noexcept;

/// Aligned plain-text table, stable ordering, no color (safe for tests/CI logs).
[[nodiscard]] std::string render_module_table(int stage_filter = -1);

/// Markdown table for docs/modules.generated.md.
[[nodiscard]] std::string render_module_table_markdown();

/// Convenience for `bonk modules --stage N`: true when `stage_filter` is -1 or equals module.stage.
[[nodiscard]] bool module_matches_stage(const ModuleInfo& module, int stage_filter) noexcept;

}  // namespace bonk
