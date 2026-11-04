// Bonk Brauzer — end-to-end pipeline used by the CLI and by integration tests.
//
//   HTML bytes -> tokenizer/tree builder -> DOM -> <style>/<link> CSS
//   -> computed styles -> layout -> raster -> PNG
//
// Keeping the pipeline in a library (not in main.cpp) means `bonk render` and the
// tests exercise exactly the same code path.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "bonk/core/diag.hpp"
#include "bonk/core/result.hpp"
#include "bonk/css/css.hpp"
#include "bonk/dom/dom.hpp"

namespace bonk::app {

struct RenderRequest {
  std::string input_path;         // local .html file
  std::string output_path = "out.png";
  float width = 1024.0f;
  float height = 768.0f;
  std::string font_path;          // empty = auto-detect (BONK_FONT / DejaVu / tofu)
  bool debug_boxes = false;
  bool dump_layout = false;
  std::string extra_css;          // additional stylesheet, applied after the page's own
};

struct RenderReport {
  std::string font_name;
  std::size_t node_count = 0;
  std::size_t css_rule_count = 0;
  std::size_t image_count = 0;  // decoded <img> bitmaps available to paint
  float document_height = 0.0f;
  std::string layout_dump;
  std::string output_path;
};

/// Reads `path` as UTF-8 text. Errors: "io.*".
[[nodiscard]] Result<std::string> read_file(std::string_view path);

/// Parses HTML + CSS from a file and lays it out. Returns the document so callers
/// can inspect the tree (`bonk dom`), plus the computed style map size.
struct ParsedPage {
  std::unique_ptr<dom::Document> document;
  css::StyleSheet sheet;  // page CSS: <style> blocks, local <link rel=stylesheet>, --css
};

[[nodiscard]] Result<ParsedPage> parse_page_from_file(std::string_view path, Diagnostics& diagnostics,
                                                      std::string_view extra_css = {});

/// Full pipeline: file -> PNG. Reports diagnostics through `diagnostics`.
[[nodiscard]] Result<RenderReport> render_file(const RenderRequest& request, Diagnostics& diagnostics);

}  // namespace bonk::app
