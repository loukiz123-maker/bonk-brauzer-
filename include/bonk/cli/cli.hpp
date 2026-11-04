// Bonk Brauzer — command line front end (parser + help text).
//
//   bonk render page.html -o page.png --width 800 --height 600
//   bonk dom page.html                 dump the parsed DOM tree
//   bonk modules --stage 2             roadmap introspection
//   bonk version --full                version + toolchain
//   bonk selftest                      runtime invariants
//
// Parsing lives in a library (not in main.cpp) so it can be unit tested without
// spawning processes.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk {

enum class Command { Help, Version, Modules, SelfTest, Render, DumpDom, Fetch };

std::string_view command_name(Command command);

struct CliOptions {
  Command command = Command::Help;
  bool full = false;          // version --full / modules --long
  int stage_filter = -1;      // modules --stage N, -1 = all
  bool markdown = false;      // modules --markdown (docs generation)

  // render / dom / fetch
  std::string input;                    // html file or URL
  std::string output = "out.png";       // render destination
  int width = 1024;
  int height = 768;
  std::string font_path;                // empty = auto-detect
  std::string css_path;                 // extra stylesheet
  bool debug_boxes = false;
  bool dump_layout = false;
  bool show_diagnostics = false;

  std::vector<std::string> positionals;
};

/// Returns a usage error (code "cli.*") instead of throwing.
[[nodiscard]] Result<CliOptions> parse_cli_args(std::span<const std::string_view> args);

[[nodiscard]] std::string cli_help_text();

/// Expands "--width=800" into {"--width", "800"}.
[[nodiscard]] std::vector<std::string> normalize_cli_args(std::span<const std::string_view> args);

}  // namespace bonk
