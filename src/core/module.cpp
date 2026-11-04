#include "bonk/core/module.hpp"

#include <algorithm>

#include "bonk/core/strings.hpp"

namespace bonk {
namespace {

// Declared in dependency-friendly order: a module never appears before the
// modules it depends on, which lets the UI/CI consume the list top-down.
const ModuleInfo kModules[] = {
    {"core", "Core utilities", "Result/Status, UTF-8, Arena, diagnostics, module registry", 0, ModuleStatus::Done, ""},
    {"net", "Networking", "WHATWG URL, DNS, TCP, HTTP/1.1, TLS, cookies, cache, streaming", 1, ModuleStatus::Planned,
     "core"},
    {"html", "HTML parser", "WHATWG tokenizer + tree builder (insertion modes, entities)", 1, ModuleStatus::Planned,
     "core"},
    {"dom", "DOM", "Node/Element/Text, querySelector, events, MutationObserver, innerHTML", 1, ModuleStatus::Planned,
     "core, html"},
    {"text", "Text & fonts", "TTF/OTF parsing, glyph raster, shaping, bidi, line breaking", 2, ModuleStatus::Planned,
     "core"},
    {"css", "CSS", "Tokenizer, parser, selectors, cascade, inheritance, computed values", 2, ModuleStatus::Planned,
     "dom"},
    {"layout", "Layout", "Box model, block/inline, floats, positioning, flexbox, grid", 2, ModuleStatus::Planned,
     "css, dom, text"},
    {"paint", "Paint", "CPU rasterizer, images, clipping, stacking contexts, layers", 2, ModuleStatus::Planned,
     "layout, text"},
    {"shell", "Browser shell", "Window, tabs, address bar, history, bookmarks, downloads, settings", 3,
     ModuleStatus::Planned, "dom, paint, net"},
    {"js", "JavaScript engine", "Lexer, parser, bytecode, VM, GC, event loop, builtins", 4, ModuleStatus::Planned,
     "core"},
    {"bindings", "JS <-> DOM bridge", "window, document, console, fetch, XHR, storage, History, Canvas 2D", 4,
     ModuleStatus::Planned, "js, dom, net"},
    {"security", "Security", "SOP, CORS, CSP, mixed content, process-per-tab sandbox, HTTPS UI", 6,
     ModuleStatus::Planned, "net, bindings, shell"},
    {"devtools", "DevTools", "Console, DOM/CSS inspector, network panel", 6, ModuleStatus::Planned,
     "dom, js, net, paint"},
};

constexpr std::size_t kModuleCount = sizeof(kModules) / sizeof(kModules[0]);

}  // namespace

std::string_view to_string(ModuleStatus status) noexcept {
  switch (status) {
    case ModuleStatus::Planned:
      return "planned";
    case ModuleStatus::InProgress:
      return "in-progress";
    case ModuleStatus::Done:
      return "done";
  }
  return "planned";
}

const std::vector<ModuleInfo>& modules() {
  static const std::vector<ModuleInfo> all(std::begin(kModules), std::end(kModules));
  return all;
}

const ModuleInfo* find_module(std::string_view id) {
  for (const ModuleInfo& module : kModules) {
    if (module.id == id) return &module;
  }
  return nullptr;
}

std::vector<const ModuleInfo*> modules_available_at(int stage) {
  std::vector<const ModuleInfo*> out;
  for (const ModuleInfo& module : kModules) {
    if (module.stage <= stage) out.push_back(&module);
  }
  return out;
}

std::string_view stage_title(int stage) noexcept {
  switch (stage) {
    case 0:
      return "Foundations: workspace, build, CI, contracts, plan";
    case 1:
      return "net + html + dom: fetch a page, dump the DOM tree";
    case 2:
      return "css + text + layout + paint: static pages on screen";
    case 3:
      return "shell: window, tabs, address bar, clicks, scrolling";
    case 4:
      return "js + bindings: interactive pages, events, fetch";
    case 5:
      return "flexbox/grid, Canvas, forms, cookies, storage";
    case 6:
      return "security, multi-process, devtools, performance";
    case 7:
      return "conformance: html5lib-tests, test262, WPT, Acid2";
    default:
      return "unknown stage";
  }
}

bool module_matches_stage(const ModuleInfo& module, int stage_filter) noexcept {
  return stage_filter < 0 || module.stage == stage_filter;
}

std::string render_module_table(int stage_filter) {
  const std::vector<std::string> headers = {"id", "stage", "status", "depends on", "title"};
  std::vector<std::vector<std::string>> rows;
  for (const ModuleInfo& module : kModules) {
    if (!module_matches_stage(module, stage_filter)) continue;
    rows.push_back({std::string(module.id), std::to_string(module.stage), std::string(to_string(module.status)),
                    module.depends_on.empty() ? std::string("-") : std::string(module.depends_on),
                    std::string(module.title)});
  }

  std::size_t widths[5] = {0, 0, 0, 0, 0};
  for (std::size_t column = 0; column < headers.size(); ++column) widths[column] = headers[column].size();
  for (const std::vector<std::string>& row : rows) {
    for (std::size_t column = 0; column < row.size(); ++column) {
      widths[column] = std::max(widths[column], row[column].size());
    }
  }

  std::size_t total = 0;
  for (const std::size_t width : widths) total += width;
  total += 2 * (headers.size() - 1);

  std::string out;
  auto append_row = [&](const std::vector<std::string>& row, bool last) {
    std::string line;
    for (std::size_t column = 0; column < row.size(); ++column) {
      line += pad_right(row[column], widths[column]);
      if (column + 1 < row.size()) line += "  ";
    }
    out += trim_right(line);  // no trailing blanks, columns still line up
    out += '\n';
    if (last) {
      out.append(total, '-');
      out += '\n';
    }
  };

  append_row(headers, true);
  for (const std::vector<std::string>& row : rows) append_row(row, false);
  return out;
}

std::string render_module_table_markdown() {
  std::string out = "| Module | Stage | Status | Depends on | Summary |\n|---|---|---|---|---|\n";
  for (const ModuleInfo& module : kModules) {
    out += "| `";
    out += module.id;
    out += "` | ";
    out += std::to_string(module.stage);
    out += " | ";
    out += to_string(module.status);
    out += " | ";
    out += module.depends_on.empty() ? "—" : std::string(module.depends_on);
    out += " | ";
    out += module.summary;
    out += " |\n";
  }
  return out;
}

}  // namespace bonk
