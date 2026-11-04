// Pixel reftests.
//
// A reftest pair is `<name>.html` and `<name>-ref.html`: two pages that must paint
// the same pixels by construction (one uses the feature under test, the other a
// boring equivalent). This is the mechanism the CSS working group uses, and it is
// the only honest way to check layout/paint without a golden image per platform.
//
// Comparison: a pixel counts as different when any channel differs by more than
// kChannelTolerance; the pair passes when at most kPixelBudgetPermille of the
// canvas differs (anti-aliasing and font hinting need that slack).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/conformance/conformance.hpp"
#include "bonk/paint/canvas.hpp"
#include "bonk/shell/shell.hpp"

namespace bonk::conformance {
namespace {

constexpr int kChannelTolerance = 8;
constexpr double kPixelBudgetPermille = 5.0;  // 0.5% of the canvas

[[nodiscard]] std::string find_font() {
  static const char* const kCandidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  };
  std::error_code ignored;
  for (const char* candidate : kCandidates) {
    if (std::filesystem::exists(candidate, ignored)) return candidate;
  }
  return {};
}

struct Render {
  bool ok = false;
  paint::Canvas canvas{1, 1};
  std::string error;
};

[[nodiscard]] Render render_page(const std::string& path, int width, int height, const std::string& font) {
  Render render;
  render.canvas = paint::Canvas{width, height};
  shell::Session session(static_cast<float>(width), static_cast<float>(height), font);
  const Status status = session.navigate("file:" + path);
  if (status.is_error()) {
    render.error = status.error().message;
    return render;
  }
  if (!session.render(render.canvas)) {
    render.error = "render failed";
    return render;
  }
  render.ok = true;
  return render;
}

}  // namespace

SuiteReport run_reftest_corpus(std::string_view directory, const SuiteOptions& options) {
  SuiteReport report;
  report.suite = "reftests";
  report.source = std::string(directory);
  if (!detail::directory_exists(directory)) return report;

  const int width = 480;
  const int height = 360;
  const std::string font = find_font();
  std::size_t missing_refs = 0;

  for (const std::string& path : detail::list_files(directory, ".html")) {
    if (path.size() >= 9 && path.compare(path.size() - 9, 9, "-ref.html") == 0) continue;  // the reference itself
    const std::string stem = path.substr(0, path.size() - 5);
    const std::string reference = stem + "-ref.html";
    if (!std::filesystem::exists(reference)) {
      ++missing_refs;
      continue;
    }
    if (options.limit != 0 && report.total >= options.limit) break;
    ++report.total;

    const std::string name = std::filesystem::path(path).filename().string();
    const Render actual = render_page(path, width, height, font);
    const Render expected = render_page(reference, width, height, font);
    if (!actual.ok || !expected.ok) {
      if (report.failures.size() < options.max_failures) {
        report.failures.push_back({name, "render failed: " + (actual.ok ? expected.error : actual.error)});
      }
      continue;
    }

    const std::vector<std::uint8_t>& a = actual.canvas.pixels();
    const std::vector<std::uint8_t>& b = expected.canvas.pixels();
    const std::size_t pixels = std::min(a.size(), b.size()) / 4;
    std::size_t different = 0;
    std::size_t first_x = 0;
    std::size_t first_y = 0;
    bool have_first = false;
    for (std::size_t i = 0; i < pixels; ++i) {
      int delta = 0;
      for (int channel = 0; channel < 4; ++channel) {
        const int left = a[i * 4 + static_cast<std::size_t>(channel)];
        const int right = b[i * 4 + static_cast<std::size_t>(channel)];
        delta = std::max(delta, std::abs(left - right));
      }
      if (delta <= kChannelTolerance) continue;
      ++different;
      if (!have_first) {
        first_x = i % static_cast<std::size_t>(width);
        first_y = i / static_cast<std::size_t>(width);
        have_first = true;
      }
    }
    const double permille = pixels == 0 ? 0.0 : 1000.0 * static_cast<double>(different) / static_cast<double>(pixels);
    if (permille <= kPixelBudgetPermille) {
      ++report.passed;
    } else if (report.failures.size() < options.max_failures) {
      char detail[192];
      std::snprintf(detail, sizeof detail, "%.1f%% of pixels differ (first at %zu,%zu)", permille / 10.0, first_x,
                    first_y);
      report.failures.push_back({name, detail});
    }
  }

  if (missing_refs != 0) report.notes.push_back("pages without a -ref pair: " + std::to_string(missing_refs));
  return report;
}

}  // namespace bonk::conformance
