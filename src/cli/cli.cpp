#include "bonk/cli/cli.hpp"

#include <cstddef>

#include "bonk/core/module.hpp"
#include "bonk/core/strings.hpp"

namespace bonk {
namespace {

Error usage_error(std::string_view message) {
  return Error::make("cli.usage", message);
}

}  // namespace

std::vector<std::string> normalize_cli_args(std::span<const std::string_view> args) {
  std::vector<std::string> out;
  out.reserve(args.size());
  for (const std::string_view arg : args) {
    const std::size_t equals = arg.find('=');
    if (arg.size() > 2 && arg.substr(0, 2) == "--" && equals != std::string_view::npos) {
      out.emplace_back(arg.substr(0, equals));
      out.emplace_back(arg.substr(equals + 1));
    } else {
      out.emplace_back(arg);
    }
  }
  return out;
}

Result<CliOptions> parse_cli_args(std::span<const std::string_view> raw_args) {
  const std::vector<std::string> args = normalize_cli_args(raw_args);
  CliOptions options;

  if (args.empty()) {
    options.command = Command::Help;
    return Result<CliOptions>::success(std::move(options));
  }

  bool command_seen = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];

    if (arg == "-h" || arg == "--help" || arg == "help") {
      options.command = Command::Help;
      return Result<CliOptions>::success(std::move(options));
    }
    if (arg == "--version" || arg == "version") {
      options.command = Command::Version;
      command_seen = true;
      continue;
    }
    if (arg == "modules") {
      options.command = Command::Modules;
      command_seen = true;
      continue;
    }
    if (arg == "selftest") {
      options.command = Command::SelfTest;
      command_seen = true;
      continue;
    }
    if (arg == "--full" || arg == "--long") {
      options.full = true;
      continue;
    }
    if (arg == "--markdown") {
      options.markdown = true;
      continue;
    }
    if (arg == "--stage") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--stage requires a value"));
      unsigned long long value = 0;
      if (!parse_u64(args[i + 1], value) || value > 7) {
        return Result<CliOptions>::failure(usage_error("--stage expects a number in 0..7"));
      }
      options.stage_filter = static_cast<int>(value);
      ++i;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      return Result<CliOptions>::failure(
          Error::make("cli.unknown_flag", "unknown option '" + arg + "' (see `bonk help`)"));
    }
    options.positionals.push_back(arg);
    if (!command_seen) {
      return Result<CliOptions>::failure(
          Error::make("cli.unknown_command", "unknown command '" + arg + "' (see `bonk help`)"));
    }
  }

  if (!command_seen) options.command = Command::Help;
  if (options.markdown && options.command != Command::Modules) {
    return Result<CliOptions>::failure(usage_error("--markdown is only valid for `bonk modules`"));
  }
  if (options.stage_filter >= 0 && options.command != Command::Modules) {
    return Result<CliOptions>::failure(usage_error("--stage is only valid for `bonk modules`"));
  }
  return Result<CliOptions>::success(std::move(options));
}

std::string cli_help_text() {
  std::string out;
  out += "Bonk Brauzer — experimental browser with its own engine\n\n";
  out += "usage: bonk <command> [options]\n\n";
  out += "commands:\n";
  out += "  modules [--stage N] [--markdown]   list engine modules and their stage\n";
  out += "  version [--full]                  print version (and toolchain)\n";
  out += "  selftest                          run fast runtime invariant checks\n";
  out += "  help                              show this help\n\n";
  out += "stages:\n";
  for (int stage = 0; stage <= 7; ++stage) {
    out += "  ";
    out += std::to_string(stage);
    out += "  ";
    out += stage_title(stage);
    out += '\n';
  }
  return out;
}

}  // namespace bonk
