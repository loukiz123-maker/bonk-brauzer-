// bonk — command line entry point.
//
// Exit codes: 0 success, 1 failed check / render error, 2 usage error.
// Scripts and CI rely on them, so they are covered by tests/cli_test.cpp.

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <signal.h>

#include "bonk/android/engine_session.hpp"
#include "bonk/android/adb_client.hpp"
#include "bonk/android/engine_state.hpp"
#include "bonk/android/launcher_plan.hpp"
#include "bonk/android/waydroid_runtime.hpp"
#include "bonk/apps/pipeline.hpp"
#include "bonk/cli/cli.hpp"
#include "bonk/core/diag.hpp"
#include "bonk/core/module.hpp"
#include "bonk/core/selftest.hpp"
#include "bonk/core/version.hpp"
#include "bonk/js/js.hpp"
#include "bonk/net/http.hpp"
#include "bonk/shell/shell.hpp"
#include "bonk/shell/x11.hpp"
#include "bonk/paint/canvas.hpp"

namespace {

void print(std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); }

int print_diagnostics(const bonk::Diagnostics& diagnostics) {
  if (diagnostics.size() == 0) return 0;
  const std::string formatted = bonk::format_diagnostics(diagnostics);
  std::fwrite(formatted.data(), 1, formatted.size(), stderr);
  return 0;
}

/// `bonk js <file.js|code>`: the CLI face of the stage-4 engine.
int run_js(const bonk::CliOptions& options) {
  std::string source;
  {
    std::ifstream file{options.input, std::ios::binary};
    if (file) {
      source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    } else {
      source = options.input;  // inline code
    }
  }

  bonk::js::Context context;
  const bonk::js::ExecutionResult result = context.evaluate(source, "<cli>");

  for (const bonk::js::ConsoleEntry& entry : context.console()) {
    const char* level = entry.level == bonk::js::ConsoleEntry::Level::Error
                            ? "error"
                            : (entry.level == bonk::js::ConsoleEntry::Level::Warn ? "warn" : "log");
    std::fprintf(stderr, "%s: %s\n", level, entry.message.c_str());
  }
  if (!result.ok) {
    std::fprintf(stderr, "bonk: uncaught %s\n", result.message.c_str());
    return 1;
  }
  if (!result.value.is_undefined()) std::printf("%s\n", bonk::js::to_string(result.value).c_str());
  return 0;
}

int run_view(const bonk::CliOptions& options) {
  bonk::shell::Session session(static_cast<float>(options.width), static_cast<float>(options.height),
                               options.font_path);
  const bonk::Status navigated = session.navigate(options.input);
  if (navigated.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", navigated.error().to_string().c_str());
    return 1;
  }
  session.set_scroll(options.scroll);
  if (options.click_x >= 0 && options.click_y >= 0) {
    const bonk::shell::ClickOutcome outcome =
        session.click(static_cast<float>(options.click_x), static_cast<float>(options.click_y));
    if (outcome.followed_link) {
      std::fprintf(stderr, "bonk: followed %s\n", outcome.href.c_str());
    }
  }

  if (!options.screenshot.empty()) {
    const bonk::Status written = session.render_to_png(options.screenshot);
    if (written.is_error()) {
      std::fprintf(stderr, "bonk: %s\n", written.error().to_string().c_str());
      return 1;
    }
    std::printf("viewed %s -> %s (%dx%d, title=\"%s\", nodes=%zu, scroll=%.0f)\n", options.input.c_str(),
                options.screenshot.c_str(), options.width, options.height, session.tab().title.c_str(),
                session.node_count(), static_cast<double>(session.scroll_y()));
    return 0;
  }

  if (!bonk::shell::x11::display_available()) {
    std::fprintf(stderr,
                 "bonk: no X11 display found ($DISPLAY is unset or the socket is missing).\n"
                 "      Use `bonk view %s --screenshot out.png` for a headless render.\n",
                 options.input.c_str());
    return 1;
  }

  bonk::Result<std::unique_ptr<bonk::shell::x11::Window>> window =
      bonk::shell::x11::Window::open(options.width, options.height, session.tab().title);
  if (window.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", window.error().to_string().c_str());
    return 1;
  }

  bonk::paint::Canvas canvas(options.width, options.height);
  (void)session.render(canvas);
  (void)window.value()->present(canvas);

  for (;;) {
    const bonk::shell::x11::Event event = window.value()->poll(500);
    switch (event.type) {
      case bonk::shell::x11::EventType::Close:
        return 0;
      case bonk::shell::x11::EventType::Expose:
        (void)session.render(canvas);
        (void)window.value()->present(canvas);
        break;
      case bonk::shell::x11::EventType::ButtonPress:
        if (event.button == 4 || event.button == 5) {
          session.scroll_by(event.button == 4 ? -60.0f : 60.0f);
        } else if (event.button == 1) {
          (void)session.click(static_cast<float>(event.x), static_cast<float>(event.y));
        } else {
          break;
        }
        (void)session.render(canvas);
        (void)window.value()->present(canvas);
        break;
      case bonk::shell::x11::EventType::KeyPress:
        switch (event.keysym) {
          case 0xFF54:  // Down
            session.scroll_by(40.0f);
            break;
          case 0xFF52:  // Up
            session.scroll_by(-40.0f);
            break;
          case 0xFF56:  // Page_Down
            session.scroll_by(static_cast<float>(options.height) * 0.9f);
            break;
          case 0xFF55:  // Page_Up
            session.scroll_by(-static_cast<float>(options.height) * 0.9f);
            break;
          case 0xFF50:  // Home
            session.set_scroll(0.0f);
            break;
          case 0xFF57:  // End
            session.set_scroll(session.max_scroll());
            break;
          default:
            break;
        }
        (void)session.render(canvas);
        (void)window.value()->present(canvas);
        break;
      default:
        break;
    }
  }
}

int run_render(const bonk::CliOptions& options) {
  bonk::app::RenderRequest request;
  request.input_path = options.input;
  request.output_path = options.output;
  request.width = static_cast<float>(options.width);
  request.height = static_cast<float>(options.height);
  request.font_path = options.font_path;
  request.debug_boxes = options.debug_boxes;
  request.dump_layout = options.dump_layout;
  if (!options.css_path.empty()) {
    const bonk::Result<std::string> css = bonk::app::read_file(options.css_path);
    if (css.is_error()) {
      std::fprintf(stderr, "bonk: %s\n", css.error().to_string().c_str());
      return 1;
    }
    request.extra_css = css.value();
  }

  bonk::Diagnostics diagnostics;
  const bonk::Result<bonk::app::RenderReport> result = bonk::app::render_file(request, diagnostics);

  if (options.show_diagnostics) print_diagnostics(diagnostics);
  if (result.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", result.error().to_string().c_str());
    return 1;
  }

  const bonk::app::RenderReport& report = result.value();
  std::string out;
  out += "rendered ";
  out += options.input;
  out += " -> ";
  out += report.output_path;
  out += "  (";
  out += std::to_string(static_cast<int>(request.width));
  out += 'x';
  out += std::to_string(static_cast<int>(request.height));
  out += ", nodes=";
  out += std::to_string(report.node_count);
  out += ", css-rules=";
  out += std::to_string(report.css_rule_count);
  out += ", font=";
  out += report.font_name;
  out += ", doc-height=";
  out += std::to_string(static_cast<int>(report.document_height));
  out += ")\n";
  print(out);
  if (options.dump_layout) print(report.layout_dump);
  return 0;
}

int run_dump_dom(const bonk::CliOptions& options) {
  bonk::Diagnostics diagnostics;
  const bonk::Result<bonk::app::ParsedPage> page = bonk::app::parse_page_from_file(options.input, diagnostics);
  if (options.show_diagnostics) print_diagnostics(diagnostics);
  if (page.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", page.error().to_string().c_str());
    return 1;
  }
  print(page.value().document->debug_tree(page.value().document->root()));
  return 0;
}

int run_fetch(const bonk::CliOptions& options) {
  const bonk::Result<bonk::net::Url> url = bonk::net::parse_url(options.input);
  if (url.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", url.error().to_string().c_str());
    return 2;
  }
  bonk::Diagnostics diagnostics;
  const bonk::Result<bonk::net::HttpResponse> response = bonk::net::http_get(url.value(), &diagnostics);
  if (options.show_diagnostics) print_diagnostics(diagnostics);
  if (response.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", response.error().to_string().c_str());
    return 1;
  }
  std::string out;
  out += "HTTP ";
  out += std::to_string(response.value().status);
  out += ' ';
  out += response.value().reason;
  out += "\nurl: ";
  out += response.value().final_url;
  out += "\n";
  for (const auto& [name, value] : response.value().headers) {
    out += name;
    out += ": ";
    out += value;
    out += "\n";
  }
  out += "\nbody: ";
  out += std::to_string(response.value().body.size());
  out += " bytes\n";
  print(out);
  return 0;
}

volatile std::sig_atomic_t waydroid_stop_requested = 0;

void request_waydroid_stop(int) noexcept { waydroid_stop_requested = 1; }

class WaydroidSignalGuard final {
 public:
  WaydroidSignalGuard() = default;
  WaydroidSignalGuard(const WaydroidSignalGuard&) = delete;
  WaydroidSignalGuard& operator=(const WaydroidSignalGuard&) = delete;

  ~WaydroidSignalGuard() {
    if (!installed_) return;
    (void)::sigaction(SIGINT, &old_interrupt_, nullptr);
    (void)::sigaction(SIGTERM, &old_terminate_, nullptr);
  }

  bool install(std::string& error) {
    struct sigaction action {};
    action.sa_handler = request_waydroid_stop;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    waydroid_stop_requested = 0;
    if (::sigaction(SIGINT, &action, &old_interrupt_) != 0) {
      error = std::string("cannot handle SIGINT: ") + std::strerror(errno);
      return false;
    }
    if (::sigaction(SIGTERM, &action, &old_terminate_) != 0) {
      const int saved_errno = errno;
      (void)::sigaction(SIGINT, &old_interrupt_, nullptr);
      error = std::string("cannot handle SIGTERM: ") + std::strerror(saved_errno);
      return false;
    }
    installed_ = true;
    return true;
  }

 private:
  struct sigaction old_interrupt_ {};
  struct sigaction old_terminate_ {};
  bool installed_ = false;
};

// Prints every observation layer separately: a running session is never
// rendered as a booted guest or a running game.
void print_waydroid_status(const bonk::android::EngineSnapshot& snapshot) {
  const std::string rendered = bonk::android::render_engine_snapshot(snapshot);
  std::fwrite(rendered.data(), 1, rendered.size(), stdout);
}

bonk::android::EngineSnapshot waydroid_snapshot(const bonk::android::WaydroidStatus& status, bool owned) {
  return bonk::android::make_engine_snapshot(status, owned);
}

// Optional, consent-gated guest observation. A failed ADB query leaves the
// guest unobserved instead of guessing; the run continues with an unobserved
// guest so that app/input layers are never silently reported as verified.
bool probe_waydroid_guest(const bonk::CliOptions& options, bonk::android::EngineSnapshot& snapshot) {
  if (options.waydroid_adb.empty()) return true;
  bonk::android::AdbConfig adb_config;
  adb_config.executable = std::filesystem::path{options.waydroid_adb};
  adb_config.serial = options.waydroid_adb_serial;
  adb_config.allow_adb_server_autostart = options.waydroid_confirm_adb;
  const bonk::Result<bonk::android::GuestStatus> guest = bonk::android::query_guest_status(adb_config);
  if (guest.is_error()) {
    snapshot.guest = bonk::android::GuestObservation{};
    snapshot.guest.detail = "guest probe failed: " + guest.error().to_string();
    return false;
  }
  snapshot.guest.state = guest.value().state;
  snapshot.guest.observed = true;
  snapshot.guest.serial = guest.value().serial;
  snapshot.guest.detail = guest.value().detail;
  return true;
}

int stop_owned_waydroid(bonk::android::EngineSession& session) {
  const bonk::Status stopped = session.stop();
  if (stopped.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", stopped.error().to_string().c_str());
    return 1;
  }
  print("Bonk-managed Waydroid session stopped.\n");
  return 0;
}

int wait_for_owned_waydroid(bonk::android::EngineSession& session) {
  print("Waydroid is running in its own UI. Press Ctrl-C to stop this Bonk-managed session.\n");
  for (;;) {
    if (waydroid_stop_requested != 0) return stop_owned_waydroid(session);
    const bonk::android::SessionStatus runtime = session.poll();
    if (runtime.state == bonk::android::ProcessState::Running) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      continue;
    }
    if (runtime.state == bonk::android::ProcessState::Exited && runtime.exit_code.has_value() &&
        *runtime.exit_code == 0) {
      print("Waydroid session exited normally.\n");
      return 0;
    }
    if (runtime.state == bonk::android::ProcessState::Failed || runtime.state == bonk::android::ProcessState::Exited) {
      std::string message = "Waydroid session process ended unexpectedly";
      if (runtime.exit_code.has_value()) message += " with exit code " + std::to_string(*runtime.exit_code);
      if (runtime.termination_signal.has_value()) {
        message += " after signal " + std::to_string(*runtime.termination_signal);
      }
      if (!runtime.message.empty()) message += ": " + runtime.message;
      std::fprintf(stderr, "bonk: %s\n", message.c_str());
      return 1;
    }
    if (runtime.state == bonk::android::ProcessState::Stopped) {
      print("Waydroid session stopped.\n");
      return 0;
    }
  }
}

bonk::android::LauncherRequest launcher_request_from(const bonk::CliOptions& options) {
  bonk::android::LauncherRequest request;
  request.start_confirmed = options.waydroid_confirm_start;
  request.open_ui = options.waydroid_show_ui;
  if (!options.waydroid_install_apk.empty()) {
    request.apk_path = std::filesystem::path{options.waydroid_install_apk};
  }
  request.install_confirmed = options.waydroid_confirm_install;
  request.package_id = options.waydroid_launch_package;
  request.probe_guest = !options.waydroid_adb.empty();
  return request;
}

// Validates the request in isolation, before a runtime status query can have
// any side effect. Malformed input must never reach the host.
int validate_waydroid_request(const bonk::CliOptions& options, const bonk::android::WaydroidConfig& config) {
  const bonk::Result<bonk::android::RuntimeCommand> status_command =
      bonk::android::make_waydroid_status_command(config);
  if (status_command.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", status_command.error().to_string().c_str());
    return 2;
  }
  if (!options.waydroid_install_apk.empty()) {
    const std::filesystem::path selected_apk{options.waydroid_install_apk};
    const bonk::Result<bonk::android::RuntimeCommand> install_command =
        bonk::android::make_waydroid_install_apk_command(config, selected_apk);
    if (install_command.is_error()) {
      std::fprintf(stderr, "bonk: %s\n", install_command.error().to_string().c_str());
      return 2;
    }
    std::error_code file_status_error;
    if (!std::filesystem::is_regular_file(selected_apk, file_status_error) || file_status_error) {
      std::fprintf(stderr, "bonk: selected APK path is not an existing regular file\n");
      return 1;
    }
    std::ifstream apk_file{selected_apk, std::ios::binary};
    if (!apk_file) {
      std::fprintf(stderr, "bonk: selected APK file is not readable\n");
      return 1;
    }
  }
  if (!options.waydroid_launch_package.empty()) {
    const bonk::Result<bonk::android::RuntimeCommand> launch_command =
        bonk::android::make_waydroid_launch_package_command(config, options.waydroid_launch_package);
    if (launch_command.is_error()) {
      std::fprintf(stderr, "bonk: %s\n", launch_command.error().to_string().c_str());
      return 2;
    }
  }
  return 0;
}

int run_waydroid(const bonk::CliOptions& options) {
  const bonk::android::WaydroidConfig config{options.waydroid_executable};
  const int validation = validate_waydroid_request(options, config);
  if (validation != 0) return validation;

  const bonk::Result<bonk::android::WaydroidStatus> initial = bonk::android::query_waydroid_status(config);
  if (initial.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", initial.error().to_string().c_str());
    return 1;
  }

  // The plan, not this function, decides the order and the consent rules.
  const bonk::android::LauncherRequest request = launcher_request_from(options);
  const bonk::Result<bonk::android::LauncherPlan> planned = bonk::android::plan_launcher(initial.value(), request);
  if (planned.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", planned.error().to_string().c_str());
    return 1;
  }
  const bonk::android::LauncherPlan& plan = planned.value();

  bonk::android::EngineSnapshot snapshot = waydroid_snapshot(initial.value(), false);
  if (!options.waydroid_adb.empty()) probe_waydroid_guest(options, snapshot);
  print_waydroid_status(snapshot);
  if (plan.steps.empty()) return 0;
  if (plan.declined_ownership) {
    print("An existing Waydroid session is already running; Bonk will not take ownership of it.\n");
  }

  bonk::android::EngineSession owned_session;
  WaydroidSignalGuard signal_guard;
  bool owns_session = false;

  for (const bonk::android::LauncherStep& step : plan.steps) {
    if (waydroid_stop_requested != 0) {
      if (owns_session) return stop_owned_waydroid(owned_session);
      return 0;
    }
    switch (step.kind) {
      case bonk::android::LauncherStepKind::StartSession: {
        std::string signal_error;
        if (!signal_guard.install(signal_error)) {
          std::fprintf(stderr, "bonk: %s\n", signal_error.c_str());
          return 1;
        }
        const bonk::Status started = bonk::android::start_waydroid_session(owned_session, config, true);
        if (started.is_error()) {
          std::fprintf(stderr, "bonk: %s\n", started.error().to_string().c_str());
          return 1;
        }
        owns_session = true;
        print("Starting the explicitly confirmed Waydroid session...\n");
        break;
      }
      case bonk::android::LauncherStepKind::WaitForSession: {
        const bonk::Result<bonk::android::WaydroidStatus> ready =
            bonk::android::wait_for_waydroid_session(owned_session, config);
        if (ready.is_error()) {
          std::fprintf(stderr, "bonk: %s\n", ready.error().to_string().c_str());
          return 1;
        }
        snapshot = waydroid_snapshot(ready.value(), true);
        if (!options.waydroid_adb.empty()) probe_waydroid_guest(options, snapshot);
        print_waydroid_status(snapshot);
        break;
      }
      case bonk::android::LauncherStepKind::ProbeGuest:
        // Already reported above; a probe-only plan ends here.
        break;
      case bonk::android::LauncherStepKind::OpenUi: {
        const bonk::Status shown = bonk::android::show_waydroid_full_ui(config);
        if (shown.is_error()) {
          std::fprintf(stderr, "bonk: %s\n", shown.error().to_string().c_str());
          return 1;
        }
        print("Waydroid full UI opened.\n");
        break;
      }
      case bonk::android::LauncherStepKind::InstallApk: {
        snapshot.app.phase = bonk::android::AppPhase::InstallRequested;
        const bonk::Status installed = bonk::android::install_waydroid_selected_apk(
            config, std::filesystem::path{step.detail}, options.waydroid_confirm_install);
        if (installed.is_error()) {
          snapshot.app.phase = bonk::android::AppPhase::InstallCommandFailed;
          print_waydroid_status(snapshot);
          std::fprintf(stderr, "bonk: %s\n", installed.error().to_string().c_str());
          return 1;
        }
        snapshot.app.phase = bonk::android::AppPhase::InstallCommandSucceeded;
        std::printf("Waydroid install command completed for %s.\n", step.detail.c_str());
        break;
      }
      case bonk::android::LauncherStepKind::LaunchPackage: {
        snapshot.app.package_id = step.detail;
        snapshot.app.phase = bonk::android::AppPhase::LaunchRequested;
        const bonk::Status launched = bonk::android::launch_waydroid_package(config, step.detail);
        if (launched.is_error()) {
          snapshot.app.phase = bonk::android::AppPhase::LaunchCommandFailed;
          print_waydroid_status(snapshot);
          std::fprintf(stderr, "bonk: %s\n", launched.error().to_string().c_str());
          return 1;
        }
        // The command succeeded; nothing inside Android confirmed the app is
        // visible, so foreground_confirmed stays false.
        snapshot.app.phase = bonk::android::AppPhase::LaunchCommandSucceeded;
        std::printf("Waydroid launch command completed for %s.\n", step.detail.c_str());
        break;
      }
      case bonk::android::LauncherStepKind::AwaitOwnedSession:
        print_waydroid_status(snapshot);
        return wait_for_owned_waydroid(owned_session);
    }
  }

  // App actions are only interesting in the summary if they were requested.
  if (snapshot.app.phase != bonk::android::AppPhase::NotRequested) print_waydroid_status(snapshot);
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  const bonk::Result<bonk::CliOptions> parsed = bonk::parse_cli_args(args);
  if (parsed.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", parsed.error().to_string().c_str());
    return 2;
  }
  const bonk::CliOptions& options = parsed.value();

  switch (options.command) {
    case bonk::Command::Help:
      print(bonk::cli_help_text());
      return 0;

    case bonk::Command::Version: {
      std::string line = std::string(bonk::kProjectName);
      line += ' ';
      line += bonk::version_string();
      line += '\n';
      if (options.full) {
        line += bonk::build_string();
        line += '\n';
      }
      print(line);
      return 0;
    }

    case bonk::Command::Modules: {
      std::string out;
      if (options.markdown) {
        out += "<!-- generated by `bonk modules --markdown`; do not edit by hand -->\n";
        out += bonk::render_module_table_markdown();
      } else {
        out += "Bonk Brauzer — engine modules\n\n";
        out += bonk::render_module_table(options.stage_filter);
        if (options.stage_filter >= 0) {
          out += "\nstage ";
          out += std::to_string(options.stage_filter);
          out += ": ";
          out += bonk::stage_title(options.stage_filter);
          out += '\n';
        } else {
          for (int stage = 0; stage <= 7; ++stage) {
            out += "\nstage ";
            out += std::to_string(stage);
            out += ": ";
            out += bonk::stage_title(stage);
          }
          out += '\n';
        }
      }
      print(out);
      return 0;
    }

    case bonk::Command::Waydroid:
      return run_waydroid(options);

    case bonk::Command::SelfTest: {
      const bonk::SelfTestReport report = bonk::run_self_test();
      std::string out;
      for (const std::string& check : report.checks) {
        out += "ok    ";
        out += check;
        out += '\n';
      }
      for (const std::string& failure : report.failures) {
        out += "FAIL  ";
        out += failure;
        out += '\n';
      }
      out += report.passed() ? "selftest: PASS\n" : "selftest: FAIL\n";
      print(out);
      return report.passed() ? 0 : 1;
    }

    case bonk::Command::Render:
      return run_render(options);

    case bonk::Command::DumpDom:
      return run_dump_dom(options);

    case bonk::Command::View:
      return run_view(options);

    case bonk::Command::Fetch:
      return run_fetch(options);

    case bonk::Command::Js:
      return run_js(options);
  }
  return 0;
}
