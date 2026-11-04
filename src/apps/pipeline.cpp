#include "bonk/apps/pipeline.hpp"

#include <fstream>
#include <cstdio>
#include <memory>
#include <sstream>

#include "bonk/core/strings.hpp"
#include "bonk/css/css.hpp"
#include "bonk/html/html.hpp"
#include "bonk/image/image.hpp"
#include "bonk/layout/layout.hpp"
#include "bonk/paint/canvas.hpp"
#include "bonk/paint/render.hpp"
#include "bonk/text/font.hpp"

namespace bonk::app {
namespace {

Error io_error(std::string_view code, std::string_view message) {
  return Error::make(code, message);
}

std::string directory_of(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) return ".";
  return std::string(path.substr(0, slash));
}

bool is_absolute_url(std::string_view href) {
  const std::size_t colon = href.find(':');
  if (colon == std::string_view::npos) return false;
  for (std::size_t i = 0; i < colon; ++i) {
    const char c = href[i];
    const bool okay = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' ||
                      c == '-' || c == '.';
    if (!okay) return false;
  }
  return true;
}

/// Collects <style> contents and local <link rel=stylesheet> files, in document order.
std::string collect_page_css(const dom::Document& document, std::string_view base_directory,
                             Diagnostics& diagnostics) {
  std::string css;
  for (const dom::NodeId node : document.get_elements_by_tag_name("*")) {
    const std::string_view tag = document.tag_name(node);
    if (tag == "style") {
      css += document.text_content(node);
      css += "\n";
      continue;
    }
    if (tag != "link") continue;
    const std::string rel = to_ascii_lower(document.attribute(node, "rel"));
    if (rel.find("stylesheet") == std::string::npos) continue;
    const std::string_view href = document.attribute(node, "href");
    if (href.empty() || is_absolute_url(href)) continue;  // remote CSS waits for the net stage
    const std::string path = directory_of(base_directory) + "/" + std::string(href);
    const Result<std::string> contents = read_file(path);
    if (contents.is_error()) {
      diagnostics.warn(SourcePos{}, "app.stylesheet_not_found: " + path);
      continue;
    }
    css += contents.value();
    css += "\n";
  }
  return css;
}

void dump_layout_into(const layout::Box& box, int depth, std::string& out) {
  out.append(static_cast<std::size_t>(depth) * 2, ' ');
  const char* kind = "block";
  switch (box.kind) {
    case layout::BoxKind::Block:
      kind = "block";
      break;
    case layout::BoxKind::Text:
      kind = "text";
      break;
    case layout::BoxKind::Image:
      kind = "image";
      break;
    case layout::BoxKind::InlineBox:
      kind = "inline";
      break;
  }
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s [%.1f,%.1f %.1fx%.1f]", kind, static_cast<double>(box.border_box.x),
                static_cast<double>(box.border_box.y), static_cast<double>(box.border_box.width),
                static_cast<double>(box.border_box.height));
  out += buffer;
  if (box.kind == layout::BoxKind::Text) {
    out += " \"";
    out += box.text;
    out += "\"";
  } else if (box.style != nullptr) {
    out += " display=";
    out += css::to_string(box.style->display);
  }
  out += '\n';
  for (const layout::Box& child : box.children) dump_layout_into(child, depth + 1, out);
}

}  // namespace

Result<std::string> read_file(std::string_view path) {
  std::ifstream file{std::string(path), std::ios::binary};
  if (!file) return Result<std::string>::failure(io_error("io.open_failed", "cannot open '" + std::string(path) + "'"));
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return Result<std::string>::success(std::move(bytes));
}

Result<ParsedPage> parse_page_from_file(std::string_view path, Diagnostics& diagnostics,
                                       std::string_view extra_css) {
  const Result<std::string> bytes = read_file(path);
  if (bytes.is_error()) return Result<ParsedPage>::failure(bytes.error());

  ParsedPage page;
  page.document = std::make_unique<dom::Document>(std::string(path));
  const Status status = html::parse_html(bytes.value(), *page.document, &diagnostics);
  if (status.is_error()) return Result<ParsedPage>::failure(status.error());

  std::string css_text = collect_page_css(*page.document, path, diagnostics);
  css_text += std::string(extra_css);

  Result<css::StyleSheet> sheet = css::StyleSheet::parse(css_text, &diagnostics);
  if (sheet.is_error()) return Result<ParsedPage>::failure(sheet.error());
  page.sheet = std::move(sheet.value());
  return Result<ParsedPage>::success(std::move(page));
}

Result<RenderReport> render_file(const RenderRequest& request, Diagnostics& diagnostics) {
  Result<ParsedPage> page = parse_page_from_file(request.input_path, diagnostics, request.extra_css);
  if (page.is_error()) return Result<RenderReport>::failure(page.error());

  const css::StyleMap styles =
      css::compute_styles(*page.value().document, page.value().sheet, request.width, &diagnostics);

  text::Font font = request.font_path.empty() ? text::Font::system_default() : text::Font{};
  if (!request.font_path.empty()) {
    Result<text::Font> loaded = text::Font::load_file(request.font_path);
    if (loaded.is_error()) return Result<RenderReport>::failure(loaded.error());
    font = std::move(loaded.value());
  }

  // Decode every local <img> once; failures are diagnostics, not fatal.
  image::ImageStore images;
  const std::string base_directory = directory_of(request.input_path);
  for (const dom::NodeId node : page.value().document->get_elements_by_tag_name("img")) {
    const std::string_view src = page.value().document->attribute(node, "src");
    if (src.empty() || is_absolute_url(src)) continue;
    std::string error;
    if (!images.add_from_file(src, base_directory, error)) {
      diagnostics.warn(SourcePos{}, "app.image_not_loaded: " + error);
    }
  }

  layout::LayoutViewport viewport{request.width, request.height};
  const layout::LayoutResult laid_out =
      layout::layout_document(*page.value().document, styles, font, viewport, &images);

  paint::Canvas canvas(static_cast<int>(request.width), static_cast<int>(request.height));
  paint::RenderOptions options;
  options.debug_boxes = request.debug_boxes;
  paint::render_layout(canvas, laid_out.root, font, options);

  if (!canvas.write_png(request.output_path)) {
    return Result<RenderReport>::failure(
        io_error("io.write_failed", "cannot write '" + request.output_path + "'"));
  }

  RenderReport report;
  report.font_name = std::string(font.name());
  report.node_count = page.value().document->node_count();
  report.css_rule_count = page.value().sheet.rules().size();
  report.image_count = images.size();
  report.document_height = laid_out.document_height;
  report.output_path = request.output_path;
  if (request.dump_layout) dump_layout_into(laid_out.root, 0, report.layout_dump);
  return Result<RenderReport>::success(std::move(report));
}

}  // namespace bonk::app
