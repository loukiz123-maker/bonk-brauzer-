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
#include "bonk/shell/isolate.hpp"
#include "bonk/cli/cli.hpp"
#include "bonk/core/diag.hpp"
#include "bonk/core/module.hpp"
#include "bonk/core/selftest.hpp"
#include "bonk/core/version.hpp"
#include "bonk/js/js.hpp"
#include "bonk/net/http.hpp"
#include "bonk/shell/shell.hpp"
#include "bonk/shell/wayland.hpp"
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


std::string format_bytes(std::size_t value) {
  if (value < 1024) return std::to_string(value) + " B";
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f kB", static_cast<double>(value) / 1024.0);
  return buffer;
}

std::string inspect_selector(const bonk::shell::Session& session, const std::string& selector) {
  const bonk::dom::Document* document = session.document();
  if (document == nullptr) return "  (no document)\n";
  const bonk::dom::NodeId node = document->query_selector(document->root(), selector);
  if (!node.valid()) return "  " + selector + ": not found\n";

  std::string tag(document->tag_name(node));
  const std::string id(document->attribute(node, "id"));
  const std::string classes(document->attribute(node, "class"));
  if (!id.empty()) tag += "#" + id;
  if (!classes.empty()) tag += "." + classes;

  const bonk::css::ComputedStyle& style = session.styles().get(*document, node);
  std::string out = "  element  " + tag + "\n";
  if (const bonk::layout::Box* box = session.box_of(node); box != nullptr) {
    char geometry[128];
    std::snprintf(geometry, sizeof(geometry), "x=%.0f y=%.0f w=%.0f h=%.0f",
                  static_cast<double>(box->border_box.x), static_cast<double>(box->border_box.y),
                  static_cast<double>(box->border_box.width), static_cast<double>(box->border_box.height));
    out += "  box      ";
    out += geometry;
    out += "\n";
  } else {
    out += "  box      (not rendered)\n";
  }
  out += "  style    display:";
  out += bonk::css::to_string(style.display);
  out += "  color:";
  out += style.color.to_string();
  out += "  background:";
  out += style.background_color.is_transparent() ? std::string("transparent") : style.background_color.to_string();
  out += "  font-size:";
  out += std::to_string(static_cast<int>(style.font_size));
  out += "px  weight:" + std::to_string(style.font_weight);
  out += "  margin:" + style.margin[0].to_string() + "/" + style.margin[3].to_string();
  out += "  padding:" + style.padding[0].to_string() + "/" + style.padding[3].to_string();
  out += "\n";
  if (!style.width.is_auto()) out += "  width    " + style.width.to_string() + "\n";
  if (!style.height.is_auto()) out += "  height   " + style.height.to_string() + "\n";
  return out;
}

int run_devtools(const bonk::CliOptions& options) {
  bonk::shell::Session session(static_cast<float>(options.width), static_cast<float>(options.height),
                               options.font_path, options.profile_dir);
  const bonk::Status navigated = session.navigate(options.input);
  if (navigated.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", navigated.error().to_string().c_str());
    return 1;
  }
  const bonk::shell::DevToolsSnapshot tools = session.devtools();

  std::string out;
  out += "url       " + tools.url + "\n";
  out += "title     " + tools.title + "\n";
  out += "origin    " + tools.origin + "\n";
  out += "security  " + std::string(bonk::security::to_string(tools.security));
  if (tools.security == bonk::security::SecurityState::Secure) out += " (TLS, certificate chain verified)";
  if (tools.security == bonk::security::SecurityState::None) out += " (local file, opaque origin)";
  out += "\n";
  out += "csp       " + (tools.csp.empty() ? std::string("none") : tools.csp) + "\n";
  out += "dom       nodes=" + std::to_string(tools.node_count) + "\n";

  out += "scripts   " + std::to_string(tools.scripts_run) + " run, " + std::to_string(tools.scripts_blocked) +
         " blocked\n";
  for (const bonk::bindings::ScriptRun& run : tools.scripts) {
    out += "  ";
    if (run.message.rfind("blocked", 0) == 0) {
      out += "blocked ";
    } else {
      out += run.skipped ? "skipped " : (run.ok ? "ok      " : "error   ");
    }
    out += run.message.empty() ? std::string("(no output)") : run.message;
    out += "\n";
  }

  out += "console   " + std::to_string(tools.console.size()) + " entr" +
         (tools.console.size() == 1 ? "y" : "ies") + "\n";
  for (const bonk::js::ConsoleEntry& entry : tools.console) {
    const char* level = entry.level == bonk::js::ConsoleEntry::Level::Error    ? "[error]"
                        : entry.level == bonk::js::ConsoleEntry::Level::Warn   ? "[warn] "
                                                                            : "[log]  ";
    out += std::string("  ") + level + " " + entry.message + "\n";
  }

  out += "network   " + std::to_string(tools.network.size()) + " request" +
         (tools.network.size() == 1 ? "" : "s") + "\n";
  for (const bonk::shell::NetworkEntry& entry : tools.network) {
    out += "  " + entry.method + " ";
    out += entry.blocked.empty() ? std::to_string(entry.status) : std::string("BLOCKED");
    out += "  " + format_bytes(entry.response_bytes);
    char timing[64];
    std::snprintf(timing, sizeof(timing), "  %7.2f ms", entry.duration_ms);
    out += timing;
    out += "  " + entry.url;
    if (!entry.blocked.empty()) out += "  (" + entry.blocked + ")";
    out += "\n";
  }

  if (!options.selector.empty()) {
    out += "inspector " + options.selector + "\n";
    out += inspect_selector(session, options.selector);
  }
  std::fputs(out.c_str(), stdout);
  return 0;
}

int run_view_isolated(const bonk::CliOptions& options) {
  bonk::shell::IsolatedRequest request;
  request.target = options.input;
  request.width = options.width;
  request.height = options.height;
  request.font_path = options.font_path;
  request.scroll = options.scroll;
  request.profile_dir = options.profile_dir;
  const bonk::shell::IsolatedResult result = bonk::shell::render_isolated(request);
  if (result.crashed) {
    std::fprintf(stderr, "bonk: tab crashed in the renderer process: %s\n", result.error.c_str());
    return 1;
  }
  if (!result.ok) {
    std::fprintf(stderr, "bonk: %s\n", result.error.empty() ? "render failed" : result.error.c_str());
    return 1;
  }
  std::ofstream file(options.screenshot, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "bonk: cannot write '%s'\n", options.screenshot.c_str());
    return 1;
  }
  file.write(reinterpret_cast<const char*>(result.png.data()), static_cast<std::streamsize>(result.png.size()));
  std::printf("viewed %s -> %s (%dx%d, title=\"%s\", isolated pid, %.0f ms)\n", options.input.c_str(),
              options.screenshot.c_str(), options.width, options.height, result.title.c_str(), result.duration_ms);
  return 0;
}

namespace {

/// The two window backends expose the same shape of API; these traits are everything
/// the generic view loop needs, so the event handling below is written once.
struct X11Backend {
  using Window = bonk::shell::x11::Window;
  using Event = bonk::shell::x11::Event;
  static constexpr auto kClose = bonk::shell::x11::EventType::Close;
  static constexpr auto kExpose = bonk::shell::x11::EventType::Expose;
  static constexpr auto kConfigure = bonk::shell::x11::EventType::Configure;
  static constexpr auto kButtonPress = bonk::shell::x11::EventType::ButtonPress;
  static constexpr auto kKeyPress = bonk::shell::x11::EventType::KeyPress;
  static bool available() { return bonk::shell::x11::display_available(); }
  static bonk::Result<std::unique_ptr<Window>> open(int width, int height, std::string_view title) {
    return Window::open(width, height, title);
  }
};

struct WaylandBackend {
  using Window = bonk::shell::wayland::Window;
  using Event = bonk::shell::wayland::Event;
  static constexpr auto kClose = bonk::shell::wayland::EventType::Close;
  static constexpr auto kExpose = bonk::shell::wayland::EventType::Expose;
  static constexpr auto kConfigure = bonk::shell::wayland::EventType::Configure;
  static constexpr auto kButtonPress = bonk::shell::wayland::EventType::ButtonPress;
  static constexpr auto kKeyPress = bonk::shell::wayland::EventType::KeyPress;
  static bool available() { return bonk::shell::wayland::display_available(); }
  static bonk::Result<std::unique_ptr<Window>> open(int width, int height, std::string_view title) {
    return Window::open(width, height, title);
  }
};

/// Opens a window through `Backend` and runs the interactive loop: scroll with the wheel
/// or the arrow/Page keys, click links with the left button, close the window to exit.
template <class Backend>
int run_window_loop(const bonk::CliOptions& options, bonk::shell::Session& session) {
  using Window = std::unique_ptr<typename Backend::Window>;
  bonk::Result<Window> window = Backend::open(options.width, options.height, session.tab().title);
  if (window.is_error()) {
    std::fprintf(stderr, "bonk: %s\n", window.error().to_string().c_str());
    return 1;
  }

  bonk::paint::Canvas canvas(options.width, options.height);
  (void)session.render(canvas);
  (void)window.value()->present(canvas);

  for (;;) {
    const typename Backend::Event event = window.value()->poll(500);
    if (event.type == Backend::kClose) return 0;
    // A resize is the compositor's suggestion: the page keeps its own viewport for
    // now, so the window is simply re-rendered at the next expose/input.
    if (event.type == Backend::kConfigure) continue;
    if (event.type == Backend::kExpose) {
      (void)session.render(canvas);
      (void)window.value()->present(canvas);
      continue;
    }
    if (event.type == Backend::kButtonPress) {
      if (event.button == 4 || event.button == 5) {
        session.scroll_by(event.button == 4 ? -60.0f : 60.0f);
      } else if (event.button == 1) {
        (void)session.click(static_cast<float>(event.x), static_cast<float>(event.y));
      } else {
        continue;
      }
      (void)session.render(canvas);
      (void)window.value()->present(canvas);
      continue;
    }
    if (event.type == Backend::kKeyPress) {
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
          continue;
      }
      (void)session.render(canvas);
      (void)window.value()->present(canvas);
    }
  }
}

}  // namespace

int run_view(const bonk::CliOptions& options) {
  if (options.isolated) return run_view_isolated(options);
  bonk::shell::Session session(static_cast<float>(options.width), static_cast<float>(options.height),
                               options.font_path, options.profile_dir);
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
    std::printf("viewed %s -> %s (%dx%d, title=\"%s\", nodes=%zu, scroll=%.0f, security=%s)\n",
                options.input.c_str(), options.screenshot.c_str(), options.width, options.height,
                session.tab().title.c_str(), session.node_count(), static_cast<double>(session.scroll_y()),
                std::string(bonk::security::to_string(session.tab().security)).c_str());
    return 0;
  }

  const std::string& backend = options.backend;
  if (backend == "x11" && !X11Backend::available()) {
    std::fprintf(stderr,
                 "bonk: no X11 display found ($DISPLAY is unset or the socket is missing).\n"
                 "      Use `bonk view %s --screenshot out.png` for a headless render.\n",
                 options.input.c_str());
    return 1;
  }
  if (backend == "wayland" && !WaylandBackend::available()) {
    std::fprintf(stderr,
                 "bonk: no Wayland compositor found ($WAYLAND_DISPLAY is unset or the socket is "
                 "missing).\n      Use `bonk view %s --screenshot out.png` for a headless render.\n",
                 options.input.c_str());
    return 1;
  }
  if (backend == "auto") {
    // A Wayland session usually has XWayland too; the native protocol is the better fit
    // (no X server round-trip per frame), so it wins when both are present.
    if (WaylandBackend::available()) return run_window_loop<WaylandBackend>(options, session);
    if (X11Backend::available()) return run_window_loop<X11Backend>(options, session);
    std::fprintf(stderr,
                 "bonk: no window backend found: $WAYLAND_DISPLAY and $DISPLAY are unset or their\n"
                 "      sockets are missing. Use `bonk view %s --screenshot out.png` for a headless "
                 "render,\n      or --backend=x11 to force X11/XWayland.\n",
                 options.input.c_str());
    return 1;
  }
  if (backend == "wayland") return run_window_loop<WaylandBackend>(options, session);
  return run_window_loop<X11Backend>(options, session);
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

    case bonk::Command::DevTools:
      return run_devtools(options);
  }
  return 0;
}
