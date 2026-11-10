#include <cctype>
#include <string_view>
#include <vector>

#include "test_framework.hpp"

#include "bonk/cli/cli.hpp"

using namespace bonk;

namespace {
Result<CliOptions> parse(std::vector<std::string_view> args) { return parse_cli_args(args); }
}  // namespace

BONK_TEST(cli, no_args_shows_help) {
  const auto options = parse({});
  REQUIRE(options.is_ok());
  CHECK_EQ(options.value().command, Command::Help);
}

BONK_TEST(cli, version) {
  const auto options = parse({"version", "--full"});
  REQUIRE(options.is_ok());
  CHECK_EQ(options.value().command, Command::Version);
  CHECK(options.value().full);
}

BONK_TEST(cli, modules_with_stage) {
  const auto options = parse({"modules", "--stage", "2"});
  REQUIRE(options.is_ok());
  CHECK_EQ(options.value().command, Command::Modules);
  CHECK_EQ(options.value().stage_filter, 2);
}

BONK_TEST(cli, equals_form_is_normalised) {
  const std::vector<std::string_view> raw = {"--stage=5", "modules"};
  const auto normalized = normalize_cli_args(raw);
  REQUIRE(normalized.size() == 3);
  CHECK_EQ(normalized[0], std::string("--stage"));
  CHECK_EQ(normalized[1], std::string("5"));

  const auto options = parse({"modules", "--stage=5"});
  REQUIRE(options.is_ok());
  CHECK_EQ(options.value().stage_filter, 5);
}

BONK_TEST(cli, markdown_flag) {
  const auto options = parse({"modules", "--markdown"});
  REQUIRE(options.is_ok());
  CHECK(options.value().markdown);
}

BONK_TEST(cli, selftest_command) {
  const auto options = parse({"selftest"});
  REQUIRE(options.is_ok());
  CHECK_EQ(options.value().command, Command::SelfTest);
}

BONK_TEST(cli, rejects_unknown_flag) {
  const auto options = parse({"modules", "--nope"});
  REQUIRE(options.is_error());
  CHECK_EQ(options.error().code, std::string("cli.unknown_flag"));
}

BONK_TEST(cli, rejects_unknown_command) {
  const auto options = parse({"frobnicate"});
  REQUIRE(options.is_error());
  CHECK_EQ(options.error().code, std::string("cli.unknown_command"));
}

// The Waydroid launcher was removed on the project owner's decision: LynxDroid
// must not depend on Waydroid. Keep it from quietly coming back.
BONK_TEST(cli, has_no_waydroid_command_or_options) {
  const auto command = parse({"waydroid"});
  REQUIRE(command.is_error());
  CHECK_EQ(command.error().code, std::string("cli.unknown_command"));

  for (const std::string_view flag : {"--waydroid", "--confirm-start", "--show-ui", "--install-apk",
                                      "--confirm-install", "--launch-package"}) {
    const auto options = parse({"modules", flag});
    REQUIRE(options.is_error());
    CHECK_EQ(options.error().code, std::string("cli.unknown_flag"));
  }

  std::string help = cli_help_text();
  for (char& c : help) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  CHECK(help.find("waydroid") == std::string::npos);
}

BONK_TEST(cli, rejects_bad_stage_values) {
  for (const char* bad : {"8", "-1", "x", ""}) {
    const auto options = parse({"modules", "--stage", bad});
    CHECK(options.is_error());
  }
  const auto missing = parse({"modules", "--stage"});
  CHECK(missing.is_error());
}

BONK_TEST(cli, stage_only_for_modules) {
  const auto options = parse({"version", "--stage", "1"});
  REQUIRE(options.is_error());
  CHECK_EQ(options.error().code, std::string("cli.usage"));
}

BONK_TEST(cli, help_flag_wins) {
  for (const char* flag : {"-h", "--help", "help"}) {
    const auto options = parse({flag});
    REQUIRE(options.is_ok());
    CHECK_EQ(options.value().command, Command::Help);
  }
}

BONK_TEST(cli, help_text_lists_all_stages) {
  const std::string text = cli_help_text();
  CHECK(text.find("modules [--stage N]") != std::string::npos);
  CHECK(text.find("usage: bonk <command>") != std::string::npos);
  for (int stage = 0; stage <= 7; ++stage) {
    CHECK(text.find("  " + std::to_string(stage) + "  ") != std::string::npos);
  }
}
