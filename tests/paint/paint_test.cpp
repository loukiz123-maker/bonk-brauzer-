#include <cstdio>
#include <fstream>
#include <string>

#include "test_framework.hpp"

#include "bonk/css/css.hpp"
#include "bonk/html/html.hpp"
#include "bonk/paint/canvas.hpp"
#include "bonk/paint/render.hpp"

using namespace bonk;

namespace {

std::string read_binary(const char* path) {
  std::ifstream file{path, std::ios::binary};
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::uint32_t read_be32(const std::string& data, std::size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return (static_cast<std::uint8_t>(data[offset]) << 24) |
         (static_cast<std::uint8_t>(data[offset + 1]) << 16) |
         (static_cast<std::uint8_t>(data[offset + 2]) << 8) | static_cast<std::uint8_t>(data[offset + 3]);
}

}  // namespace

BONK_TEST(canvas, initialises_and_clears) {
  paint::Canvas canvas(20, 10);
  CHECK_EQ(canvas.width(), 20);
  CHECK_EQ(canvas.height(), 10);
  CHECK_EQ(canvas.pixels().size(), std::size_t{20 * 10 * 4});
  canvas.clear(css::Color{10, 20, 30, 255});
  const css::Color pixel = canvas.pixel_at(5, 5);
  CHECK_EQ(static_cast<int>(pixel.r), 10);
  CHECK_EQ(static_cast<int>(pixel.g), 20);
  CHECK_EQ(static_cast<int>(pixel.b), 30);
}

BONK_TEST(canvas, fill_rect_clips_to_the_canvas) {
  paint::Canvas canvas(20, 20);
  canvas.clear(css::Color{255, 255, 255, 255});
  canvas.fill_rect(paint::Rect{-50.0f, -50.0f, 60.0f, 60.0f}, css::Color{255, 0, 0, 255});
  CHECK_EQ(static_cast<int>(canvas.pixel_at(0, 0).r), 255);
  CHECK_EQ(static_cast<int>(canvas.pixel_at(0, 0).g), 0);
  CHECK_EQ(static_cast<int>(canvas.pixel_at(9, 9).g), 0);   // still inside the rect
  CHECK_EQ(static_cast<int>(canvas.pixel_at(15, 15).g), 255);  // untouched area

  canvas.fill_rect(paint::Rect{100.0f, 100.0f, 50.0f, 50.0f}, css::Color{0, 0, 255, 255});
  CHECK_EQ(static_cast<int>(canvas.pixel_at(5, 5).b), 0);  // off-canvas fill is a no-op
}

BONK_TEST(canvas, stroke_and_alpha_blending) {
  paint::Canvas canvas(10, 10);
  canvas.clear(css::Color{0, 0, 0, 255});
  canvas.stroke_rect(paint::Rect{0.0f, 0.0f, 10.0f, 10.0f}, css::Color{255, 255, 255, 255}, 1.0f);
  CHECK_EQ(static_cast<int>(canvas.pixel_at(0, 0).r), 255);
  CHECK_EQ(static_cast<int>(canvas.pixel_at(5, 5).r), 0);

  canvas.fill_rect(paint::Rect{2.0f, 2.0f, 2.0f, 2.0f}, css::Color{255, 255, 255, 128});
  const int blended = static_cast<int>(canvas.pixel_at(2, 2).r);
  CHECK(blended > 100);
  CHECK(blended < 200);
}

BONK_TEST(canvas, png_writer_emits_a_valid_file) {
  paint::Canvas canvas(16, 8);
  canvas.clear(css::Color{200, 100, 50, 255});
  canvas.fill_rect(paint::Rect{4.0f, 2.0f, 4.0f, 4.0f}, css::Color{0, 0, 0, 255});
  const char* path = "/tmp/bonk_canvas_test.png";
  REQUIRE(canvas.write_png(path));

  const std::string png = read_binary(path);
  REQUIRE(png.size() > 60);
  CHECK_EQ(png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n"));
  CHECK_EQ(png.substr(12, 4), std::string("IHDR"));
  CHECK_EQ(static_cast<int>(read_be32(png, 16)), 16);  // width
  CHECK_EQ(static_cast<int>(read_be32(png, 20)), 8);   // height
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(png[24])), 8);  // bit depth
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(png[25])), 6);  // RGBA
  CHECK(png.find("IDAT") != std::string::npos);
  CHECK(png.find("IEND") != std::string::npos);

  // zlib header of the IDAT payload
  const std::size_t idat = png.find("IDAT");
  REQUIRE(idat != std::string::npos);
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(png[idat + 4])), 0x78);
}

BONK_TEST(canvas, ppm_writer) {
  paint::Canvas canvas(4, 2);
  canvas.clear(css::Color{1, 2, 3, 255});
  REQUIRE(canvas.write_ppm("/tmp/bonk_canvas_test.ppm"));
  const std::string ppm = read_binary("/tmp/bonk_canvas_test.ppm");
  const std::string header = "P6\n4 2\n255\n";
  CHECK_EQ(ppm.substr(0, header.size()), header);
  CHECK_EQ(ppm.size(), header.size() + 4 * 2 * 3);
}

BONK_TEST(canvas, draw_text_produces_ink) {
  paint::Canvas canvas(120, 40);
  canvas.clear(css::Color{255, 255, 255, 255});
  const text::Font font;
  const float pen = canvas.draw_text(font, "Bonk", 4.0f, 30.0f, 24.0f, css::Color{0, 0, 0, 255});
  CHECK(pen > 4.0f);

  int dark_pixels = 0;
  for (int y = 0; y < canvas.height(); ++y) {
    for (int x = 0; x < canvas.width(); ++x) {
      if (canvas.pixel_at(x, y).r < 200) ++dark_pixels;
    }
  }
  CHECK(dark_pixels > 20);
}

BONK_TEST(render, paints_backgrounds_borders_and_text) {
  Diagnostics diagnostics;
  dom::Document document;
  REQUIRE(html::parse_html("<div id=\"box\">Hello</div>",
                           document, &diagnostics)
              .is_ok());
  Result<css::StyleSheet> sheet = css::StyleSheet::parse(
      "div { display: block; background-color: #ff0000; height: 40px; border: 2px solid #0000ff;"
      " font-size: 20px; color: #000000 }",
      &diagnostics);
  REQUIRE(sheet.is_ok());
  const css::StyleMap styles = css::compute_styles(document, sheet.value(), 200.0f, &diagnostics);
  const layout::LayoutResult laid_out =
      layout::layout_document(document, styles, text::Font{}, layout::LayoutViewport{200.0f, 100.0f});

  paint::Canvas canvas(200, 100);
  paint::RenderOptions options;
  paint::render_layout(canvas, laid_out.root, text::Font{}, options);

  CHECK_EQ(static_cast<int>(canvas.pixel_at(100, 20).r), 255);  // red background
  CHECK_EQ(static_cast<int>(canvas.pixel_at(100, 20).g), 0);
  CHECK_EQ(static_cast<int>(canvas.pixel_at(0, 20).b), 255);  // blue border on the left edge
  CHECK_EQ(static_cast<int>(canvas.pixel_at(150, 90).g), 255);  // page background stays white
}

BONK_TEST(render, debug_boxes_overlay_is_optional) {
  Diagnostics diagnostics;
  dom::Document document;
  REQUIRE(html::parse_html("<div style=\"height: 20px\">x</div>", document, &diagnostics).is_ok());
  Result<css::StyleSheet> sheet = css::StyleSheet::parse("", &diagnostics);
  REQUIRE(sheet.is_ok());
  const css::StyleMap styles = css::compute_styles(document, sheet.value(), 100.0f, &diagnostics);
  const layout::LayoutResult laid_out =
      layout::layout_document(document, styles, text::Font{}, layout::LayoutViewport{100.0f, 50.0f});

  paint::Canvas plain(100, 50);
  paint::render_layout(plain, laid_out.root, text::Font{}, paint::RenderOptions{});

  paint::Canvas debugged(100, 50);
  paint::RenderOptions options;
  options.debug_boxes = true;
  paint::render_layout(debugged, laid_out.root, text::Font{}, options);

  CHECK(plain.pixel_at(0, 0) != debugged.pixel_at(0, 0));
}
