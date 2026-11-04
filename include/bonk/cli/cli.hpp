// Bonk Brauzer — command line front end (parser + help text).
//
// The CLI is the primary development driver until the shell exists:
//   bonk modules --stage 1     what stage 1 is about
//   bonk version --full        version + toolchain
//   bonk selftest              cheap runtime invariants (no test framework)
//
// Parsing lives in a library (not in main.cpp) so it can be unit tested
// without spawning processes.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk {

enum class Command { Help, Version, Modules, SelfTest };

struct CliOptions {
  Command command = Command::Help;
  bool full = false;         // version --full / modules --long
  int stage_filter = -1;     // modules --stage N, -1 = all
  bool markdown = false;     // modules --markdown (docs generation)
  std::vector<std::string> positionals;
};

/// Returns a usage error (code "cli.*") instead of throwing.
[[nodiscard]] Result<CliOptions> parse_cli_args(std::span<const std::string_view> args);

[[nodiscard]] std::string cli_help_text();

/// Expand "--stage=2" into {"--stage", "2"} and split "--stage 2" stays as is.
/// Exposed for tests; used internally by parse_cli_args.
[[nodiscard]] std::vector<std::string> normalize_cli_args(std::span<const std::string_view> args);

}  // namespace bonk
