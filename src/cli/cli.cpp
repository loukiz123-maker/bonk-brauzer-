#include "bonk/cli/cli.hpp"

#include <cstddef>
#include <cstdlib>

#include "bonk/core/module.hpp"
#include "bonk/core/strings.hpp"

namespace bonk {
namespace {

Error usage_error(std::string_view message) { return Error::make("cli.usage", message); }

bool parse_int_arg(std::string_view text, int& out) {
  unsigned long long value = 0;
  if (!parse_u64(text, value) || value > 16384) return false;
  out = static_cast<int>(value);
  return true;
}

struct CommandEntry {
  const char* name;
  Command command;
};

const CommandEntry kCommands[] = {
    {"help", Command::Help},         {"version", Command::Version}, {"modules", Command::Modules},
    {"selftest", Command::SelfTest}, {"render", Command::Render},   {"dom", Command::DumpDom},
    {"fetch", Command::Fetch},       {"view", Command::View},      {"js", Command::Js},
};

}  // namespace

std::string_view command_name(Command command) {
  for (const CommandEntry& entry : kCommands) {
    if (entry.command == command) return entry.name;
  }
  return "help";
}

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

    if (arg == "-h" || arg == "--help") {
      options.command = Command::Help;
      return Result<CliOptions>::success(std::move(options));
    }
    if (arg == "--full" || arg == "--long") {
      options.full = true;
      continue;
    }
    if (arg == "--markdown") {
      options.markdown = true;
      continue;
    }
    if (arg == "--debug-boxes") {
      options.debug_boxes = true;
      continue;
    }
    if (arg == "--dump-layout") {
      options.dump_layout = true;
      continue;
    }
    if (arg == "--diagnostics") {
      options.show_diagnostics = true;
      continue;
    }
    if (arg == "-o" || arg == "--out") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--out requires a path"));
      options.output = args[++i];
      continue;
    }
    if (arg == "--width" || arg == "--height") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error(arg + " requires a value"));
      int value = 0;
      if (!parse_int_arg(args[i + 1], value) || value <= 0) {
        return Result<CliOptions>::failure(usage_error(arg + " expects a positive integer"));
      }
      if (arg == "--width") {
        options.width = value;
      } else {
        options.height = value;
      }
      ++i;
      continue;
    }
    if (arg == "--font") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--font requires a path"));
      options.font_path = args[++i];
      continue;
    }
    if (arg == "--css") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--css requires a path"));
      options.css_path = args[++i];
      continue;
    }
    if (arg == "--screenshot") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--screenshot requires a path"));
      options.screenshot = args[++i];
      continue;
    }
    if (arg == "--scroll") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--scroll requires a value"));
      int value = 0;
      if (!parse_int_arg(args[i + 1], value) || value < 0) {
        return Result<CliOptions>::failure(usage_error("--scroll expects a non-negative integer"));
      }
      options.scroll = static_cast<float>(value);
      ++i;
      continue;
    }
    if (arg == "--click") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--click requires X,Y"));
      const std::string& spec = args[i + 1];
      const std::size_t comma = spec.find(',');
      int x = 0;
      int y = 0;
      if (comma == std::string::npos || !parse_int_arg(spec.substr(0, comma), x) ||
          !parse_int_arg(spec.substr(comma + 1), y)) {
        return Result<CliOptions>::failure(usage_error("--click expects X,Y (e.g. --click 40,60)"));
      }
      options.click_x = x;
      options.click_y = y;
      ++i;
      continue;
    }
    if (arg == "--stage") {
      if (i + 1 >= args.size()) return Result<CliOptions>::failure(usage_error("--stage requires a value"));
      int value = 0;
      if (!parse_int_arg(args[i + 1], value) || value > 7) {
        return Result<CliOptions>::failure(usage_error("--stage expects a number in 0..7"));
      }
      options.stage_filter = value;
      ++i;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      return Result<CliOptions>::failure(Error::make("cli.unknown_flag", "unknown option '" + arg + "' (see `bonk help`)"));
    }

    if (!command_seen) {
      bool known = false;
      for (const CommandEntry& entry : kCommands) {
        if (arg == entry.name) {
          options.command = entry.command;
          known = true;
          break;
        }
      }
      if (!known) {
        return Result<CliOptions>::failure(
            Error::make("cli.unknown_command", "unknown command '" + arg + "' (see `bonk help`)"));
      }
      command_seen = true;
      continue;
    }
    options.positionals.push_back(arg);
  }

  if (!command_seen) options.command = Command::Help;

  const std::size_t wanted_positionals = (options.command == Command::Render || options.command == Command::DumpDom ||
                                          options.command == Command::Fetch || options.command == Command::View ||
                                          options.command == Command::Js)
                                             ? 1
                                             : 0;
  if (options.positionals.size() != wanted_positionals) {
    if (wanted_positionals == 1) {
      return Result<CliOptions>::failure(usage_error("`bonk " + std::string(command_name(options.command)) +
                                                     "` requires exactly one input path or URL"));
    }
    return Result<CliOptions>::failure(
        usage_error("`bonk " + std::string(command_name(options.command)) + "` takes no positional arguments"));
  }
  if (!options.positionals.empty()) options.input = options.positionals.front();

  if (options.markdown && options.command != Command::Modules) {
    return Result<CliOptions>::failure(usage_error("--markdown is only valid for `bonk modules`"));
  }
  if (options.stage_filter >= 0 && options.command != Command::Modules) {
    return Result<CliOptions>::failure(usage_error("--stage is only valid for `bonk modules`"));
  }
  if (options.command != Command::Render) {
    if (options.debug_boxes || options.dump_layout) {
      return Result<CliOptions>::failure(usage_error("--debug-boxes/--dump-layout are only valid for `bonk render`"));
    }
  }
  return Result<CliOptions>::success(std::move(options));
}

std::string cli_help_text() {
  std::string out;
  out += "Bonk Brauzer — experimental browser with its own engine\n\n";
  out += "usage: bonk <command> [options]\n\n";
  out += "commands:\n";
  out += "  render <file.html> [-o out.png] [--width N] [--height N] [--font PATH]\n";
  out += "                      [--css PATH] [--debug-boxes] [--dump-layout] [--diagnostics]\n";
  out += "                    parse, style, lay out and rasterise a local page to PNG\n";
  out += "  dom <file.html>   print the parsed DOM tree\n";
  out += "  fetch <url>       HTTP/1.1 GET and print the response head (TLS is not ready yet)\n";
  out += "  view <page>       open the page in an X11 window (own X11 client, no Xlib)\n";
  out += "                    --screenshot out.png  render headlessly instead\n";
  out += "                    --scroll N            start scrolled down N pixels\n";
  out += "                    --click X,Y           click before screenshotting\n";
  out += "  js <file.js|code> run a script in the engine (own lexer, parser, interpreter)\n";
  out += "  modules [--stage N] [--markdown]   list engine modules and their stage\n";
  out += "  version [--full]  print version (and toolchain)\n";
  out += "  selftest          run fast runtime invariant checks\n";
  out += "  help              show this help\n\n";
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
