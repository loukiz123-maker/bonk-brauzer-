// Bonk Brauzer — command line front end (parser + help text).
//
//   bonk view page.html                open the page in an X11 window (own X11 client)
//   bonk view page.html --screenshot p.png --scroll 200 --click 40,60
//   bonk render page.html -o page.png --width 800 --height 600
//   bonk dom page.html                 dump the parsed DOM tree
//   bonk js script.js                  run JavaScript in the engine (file or inline code)
//   bonk modules --stage 2             roadmap introspection
//   bonk version --full                version + toolchain
//   bonk selftest                      runtime invariants
//   bonk waydroid --confirm-start      launch an installed Waydroid session
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

enum class Command { Help, Version, Modules, SelfTest, Render, DumpDom, Fetch, View, Js, Waydroid };

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

  // view
  std::string screenshot;   // non-empty: render headlessly instead of opening a window
  float scroll = 0.0f;      // initial scroll offset (CSS px)
  int click_x = -1;         // >= 0 together with click_y: click before screenshotting
  int click_y = -1;

  // Waydroid Linux gaming runtime. Starts a host-owned Android session only
  // after explicit confirmation; optional APK installation needs its own flag.
  std::string waydroid_executable = "/usr/bin/waydroid";
  bool waydroid_executable_explicit = false;
  std::string waydroid_install_apk;
  std::string waydroid_launch_package;
  bool waydroid_confirm_start = false;
  bool waydroid_confirm_install = false;
  bool waydroid_show_ui = false;

  std::vector<std::string> positionals;
};

/// Returns a usage error (code "cli.*") instead of throwing.
[[nodiscard]] Result<CliOptions> parse_cli_args(std::span<const std::string_view> args);

[[nodiscard]] std::string cli_help_text();

/// Expands "--width=800" into {"--width", "800"}.
[[nodiscard]] std::vector<std::string> normalize_cli_args(std::span<const std::string_view> args);

}  // namespace bonk
