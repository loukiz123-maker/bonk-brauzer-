#include <set>
#include <string>

#include "test_framework.hpp"

#include "bonk/core/module.hpp"
#include "bonk/core/strings.hpp"

using namespace bonk;

BONK_TEST(module, registry_contents) {
  const std::vector<ModuleInfo>& all = modules();
  CHECK_EQ(all.size(), std::size_t{15});

  // Every module from the specification is present exactly once.
  const char* expected[] = {"core", "crypto", "net",      "html",     "dom",      "css",     "layout",
                            "paint", "text",  "image",    "js",       "bindings", "shell",   "security",
                            "devtools"};
  for (const char* id : expected) CHECK(find_module(id) != nullptr);

  std::set<std::string_view> ids;
  for (const ModuleInfo& module : all) CHECK(ids.insert(module.id).second);
}

BONK_TEST(module, order_respects_dependencies) {
  // A module may only depend on modules declared earlier in the table.
  std::set<std::string_view> seen;
  for (const ModuleInfo& module : modules()) {
    for (const std::string_view dependency : split(module.depends_on, ',')) {
      const std::string_view id = trim(dependency);
      if (id.empty()) continue;
      CHECK(find_module(id) != nullptr);
      CHECK(seen.count(id) == 1);
    }
    seen.insert(module.id);
  }
}

BONK_TEST(module, stage_zero_only_core_is_done) {
  for (const ModuleInfo& module : modules()) {
    if (module.stage == 0) {
      CHECK_EQ(module.status, ModuleStatus::Done);
    } else {
      CHECK_NE(module.status, ModuleStatus::Done);
    }
  }
}

BONK_TEST(module, stages_one_and_two_are_in_progress) {
  for (const ModuleInfo& module : modules()) {
    if (module.stage == 1 || module.stage == 2) CHECK_EQ(module.status, ModuleStatus::InProgress);
    // Stage 3 started with the shell + X11 backend; everything later is still planned.
    if (module.stage == 3 && module.id == "shell") CHECK_EQ(module.status, ModuleStatus::InProgress);
    if (module.stage >= 4) CHECK_EQ(module.status, ModuleStatus::Planned);
  }
}

BONK_TEST(module, available_at_stage_is_cumulative) {
  const auto stage0 = modules_available_at(0);
  REQUIRE(stage0.size() == 1);
  CHECK_EQ(stage0[0]->id, std::string_view("core"));

  const auto stage1 = modules_available_at(1);
  CHECK_EQ(stage1.size(), std::size_t{5});

  const auto stage7 = modules_available_at(7);
  CHECK_EQ(stage7.size(), modules().size());
}

BONK_TEST(module, render_table_is_aligned_and_deterministic) {
  const std::string table = render_module_table();
  CHECK_EQ(table, render_module_table());

  const auto lines = split(table, '\n');
  REQUIRE(!lines.empty());
  CHECK_EQ(std::string(table.substr(0, 2)), std::string("id"));

  // Header + separator + 13 rows + trailing empty line.
  CHECK_EQ(lines.size(), std::size_t{18});
  REQUIRE(lines[1].size() > 40);
  const std::string_view separator = lines[1];
  CHECK(separator.find_first_not_of('-') == std::string_view::npos);  // separator is a dashed rule
  CHECK_EQ(std::string(separator).size(), separator.size());

  const std::size_t width = separator.size();  // full table width
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].empty()) continue;
    CHECK(lines[i].size() <= width);  // no row overflows, trailing blanks are trimmed
  }
  CHECK_EQ(lines[0].rfind("id", 0), std::size_t{0});
  CHECK_EQ(lines[2].rfind("core", 0), std::size_t{0});
  CHECK(lines[2].find("Core utilities") != std::string::npos);
  CHECK(table.find("core") != std::string::npos);  // last column is not padded
}

BONK_TEST(module, render_table_stage_filter) {
  const std::string table = render_module_table(2);
  CHECK(table.find("css") != std::string::npos);
  CHECK(table.find("paint") != std::string::npos);
  CHECK(table.find("devtools") == std::string::npos);
  CHECK(render_module_table(42).find("id") == 0);  // header only, still valid
}

BONK_TEST(module, markdown_table) {
  const std::string markdown = render_module_table_markdown();
  CHECK(markdown.rfind("| Module | Stage | Status | Depends on | Summary |", 0) == 0);
  CHECK(markdown.find("| `html` | 1 | in-progress | core |") != std::string::npos);
  CHECK(markdown.find("| `core` | 0 | done | — |") != std::string::npos);
}

BONK_TEST(module, stage_titles) {
  CHECK_EQ(stage_title(0), std::string_view("Foundations: workspace, build, CI, contracts, plan"));
  CHECK_EQ(stage_title(7), std::string_view("conformance: html5lib-tests, test262, WPT, Acid2"));
  CHECK_EQ(stage_title(99), std::string_view("unknown stage"));
}

BONK_TEST(module, status_names) {
  CHECK_EQ(to_string(ModuleStatus::Planned), std::string_view("planned"));
  CHECK_EQ(to_string(ModuleStatus::InProgress), std::string_view("in-progress"));
  CHECK_EQ(to_string(ModuleStatus::Done), std::string_view("done"));
}
