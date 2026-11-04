// bonk — command line entry point.
//
// Exit codes: 0 success, 1 failed check / render error, 2 usage error.
// Scripts and CI rely on them, so they are covered by tests/cli_test.cpp.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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
