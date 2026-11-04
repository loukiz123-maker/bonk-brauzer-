#include <cstdio>
#include <fstream>
#include <string>

#include "test_framework.hpp"

#include "bonk/apps/pipeline.hpp"

using namespace bonk;

namespace {

std::string read_binary(const std::string& path) {
  std::ifstream file{path, std::ios::binary};
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
  std::ifstream file{path};
  return static_cast<bool>(file);
}

}  // namespace

BONK_TEST(pipeline, read_file) {
  REQUIRE(file_exists("tests/fixtures/sample.html"));
  const Result<std::string> text = app::read_file("tests/fixtures/sample.html");
  REQUIRE(text.is_ok());
  CHECK(text.value().find("<h1>Bonk Brauzer</h1>") != std::string::npos);

  const Result<std::string> missing = app::read_file("/definitely/not/here.html");
  REQUIRE(missing.is_error());
  CHECK_EQ(missing.error().code, std::string("io.open_failed"));
}

BONK_TEST(pipeline, parse_page_collects_dom_and_css) {
  Diagnostics diagnostics;
  const Result<app::ParsedPage> page = app::parse_page_from_file("tests/fixtures/sample.html", diagnostics);
  REQUIRE(page.is_ok());
  CHECK(page.value().document->node_count() > 40);
  CHECK(page.value().sheet.rules().size() >= 10);

  const dom::NodeId heading = page.value().document->query_selector(page.value().document->root(), "h1");
  REQUIRE(heading.valid());
  CHECK_EQ(page.value().document->text_content(heading), std::string("Bonk Brauzer"));

  const dom::NodeId card = page.value().document->query_selector(page.value().document->root(), ".card");
  CHECK(card.valid());
}

BONK_TEST(pipeline, render_writes_a_png) {
  app::RenderRequest request;
  request.input_path = "tests/fixtures/sample.html";
  request.output_path = "/tmp/bonk_pipeline_test.png";
  request.width = 640.0f;
  request.height = 480.0f;

  Diagnostics diagnostics;
  const Result<app::RenderReport> report = app::render_file(request, diagnostics);
  REQUIRE(report.is_ok());
  CHECK(report.value().node_count > 40);
  CHECK(report.value().css_rule_count >= 10);
  CHECK(report.value().document_height > 100.0f);
  CHECK(!report.value().font_name.empty());

  const std::string png = read_binary(request.output_path);
  REQUIRE(png.size() > 1000);
  CHECK_EQ(png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n"));

  std::remove(request.output_path.c_str());
}

BONK_TEST(pipeline, render_with_extra_css_and_layout_dump) {
  app::RenderRequest request;
  request.input_path = "tests/fixtures/sample.html";
  request.output_path = "/tmp/bonk_pipeline_extra.png";
  request.width = 320.0f;
  request.height = 240.0f;
  request.extra_css = "body { background-color: #123456; margin: 0 } h1 { display: none }";
  request.dump_layout = true;

  Diagnostics diagnostics;
  const Result<app::RenderReport> report = app::render_file(request, diagnostics);
  REQUIRE(report.is_ok());
  CHECK(report.value().layout_dump.find("block") != std::string::npos);
  CHECK(report.value().layout_dump.find("\"Bonk\"") == std::string::npos);  // h1 is hidden
  std::remove(request.output_path.c_str());
}

BONK_TEST(pipeline, missing_file_is_reported) {
  app::RenderRequest request;
  request.input_path = "/no/such/page.html";
  request.output_path = "/tmp/never.png";

  Diagnostics diagnostics;
  const Result<app::RenderReport> report = app::render_file(request, diagnostics);
  REQUIRE(report.is_error());
  CHECK_EQ(report.error().code, std::string("io.open_failed"));
}

BONK_TEST(pipeline, loads_local_images_into_the_layout) {
  // Reuse the checked-in fixture: logo.png decodes, nope.png stays a placeholder.
  app::RenderRequest request;
  request.input_path = "tests/fixtures/images.html";
  request.output_path = "/tmp/bonk-pipeline-images.png";
  request.width = 420;
  request.height = 300;
  request.dump_layout = true;

  Diagnostics diagnostics;
  const Result<app::RenderReport> rendered = app::render_file(request, diagnostics);
  REQUIRE(rendered.is_ok());
  CHECK_EQ(rendered.value().image_count, std::size_t{1});
  CHECK(rendered.value().layout_dump.find("image") != std::string::npos);
  CHECK(!diagnostics.items().empty());  // the missing file is reported, not fatal
}
