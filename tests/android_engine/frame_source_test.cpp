#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "bonk/android/frame_source.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace bonk::android;

namespace {

std::string shell_quote(std::string_view value) {
  std::string quoted;
  quoted.push_back('\'');
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

/// Fake ADB that serves a fixed PNG for `exec-out screencap -p`.
class FakeScreenshotAdb final {
 public:
  explicit FakeScreenshotAdb(std::string_view fixture = "tests/fixtures/logo.png",
                             std::string_view body = {}) {
    std::string pattern = (std::filesystem::temp_directory_path() / "bonk-frame-adb-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const char* directory = ::mkdtemp(mutable_pattern.data());
    if (directory == nullptr) return;
    directory_ = directory;
    executable_ = directory_ / "adb-fake";
    std::ofstream script(executable_);
    if (!script) return;
    if (!body.empty()) {
      const std::filesystem::path payload = directory_ / "payload.bin";
      std::ofstream out{payload, std::ios::binary};
      out << body;
      out.close();
      script << "#!/bin/sh\nexec /bin/cat " << shell_quote(payload.string()) << "\n";
    } else {
      const std::filesystem::path fixture_path =
          std::filesystem::absolute(std::string(fixture));
      script << "#!/bin/sh\nexec /bin/cat " << shell_quote(fixture_path.string()) << "\n";
    }
    script.close();
    ::chmod(executable_.c_str(), 0755);
    ready_ = true;
  }

  ~FakeScreenshotAdb() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] AdbConfig config(bool consent = true) const {
    AdbConfig config;
    config.executable = executable_;
    config.serial = "emulator-5554";
    config.allow_adb_server_autostart = consent;
    return config;
  }

 private:
  std::filesystem::path directory_;
  std::filesystem::path executable_;
  bool ready_ = false;
};

}  // namespace

BONK_TEST(frame_source, still_capture_reports_observed_geometry) {
  FakeScreenshotAdb fake;
  REQUIRE(fake.ready());

  const Result<StillFrame> frame = capture_still_frame(fake.config(), 7);
  REQUIRE(frame.is_ok());
  CHECK_EQ(static_cast<int>(frame.value().metadata.width), frame.value().image.width);
  CHECK_EQ(static_cast<int>(frame.value().metadata.height), frame.value().image.height);
  CHECK(frame.value().metadata.width > 0);
  CHECK_EQ(frame.value().metadata.stride, frame.value().metadata.width * 4);
  CHECK_EQ(frame.value().metadata.sequence, std::uint64_t{7});
  CHECK(frame.value().metadata.format == FramePixelFormat::Rgba8);
  CHECK(frame.value().metadata.source == FrameSourceKind::StillSnapshot);
  CHECK(frame.value().metadata.capture_time_ns > 0);
  // The buffer really has stride*height bytes for a tightly packed RGBA image.
  CHECK_EQ(frame.value().image.rgba.size(),
           static_cast<std::size_t>(frame.value().metadata.stride) * frame.value().metadata.height);
}

BONK_TEST(frame_source, rotation_is_unknown_not_zero) {
  FakeScreenshotAdb fake;
  REQUIRE(fake.ready());

  const Result<StillFrame> frame = capture_still_frame(fake.config(), 1);
  REQUIRE(frame.is_ok());
  // A screenshot PNG cannot report orientation; defaulting to 0 would lie.
  CHECK(!frame.value().metadata.rotation_degrees.has_value());
  CHECK(render_frame_metadata(frame.value().metadata).find("rotation=unknown") != std::string::npos);
}

BONK_TEST(frame_source, a_still_frame_is_never_reported_as_streaming) {
  FakeScreenshotAdb fake;
  REQUIRE(fake.ready());

  const Result<StillFrame> frame = capture_still_frame(fake.config(), 1);
  REQUIRE(frame.is_ok());
  CHECK(frame.value().metadata.source != FrameSourceKind::Streaming);
  const std::string rendered = render_frame_metadata(frame.value().metadata);
  CHECK(rendered.find("frame source=still-snapshot") == 0);
  CHECK(rendered.find("streaming") == std::string::npos);
}

BONK_TEST(frame_source, capture_requires_adb_consent) {
  FakeScreenshotAdb fake;
  REQUIRE(fake.ready());

  const Result<StillFrame> frame = capture_still_frame(fake.config(false), 1);
  REQUIRE(frame.is_error());
  CHECK_EQ(frame.error().code, std::string("android.adb_server_consent_required"));
}

BONK_TEST(frame_source, undecodable_payload_fails_instead_of_returning_a_frame) {
  FakeScreenshotAdb fake{"", "this is not a png"};
  REQUIRE(fake.ready());

  const Result<StillFrame> frame = capture_still_frame(fake.config(), 1);
  REQUIRE(frame.is_error());
  CHECK(frame.error().code.rfind("android.", 0) == 0);
}

BONK_TEST(frame_source, metadata_validation_rejects_impossible_geometry) {
  FrameMetadata metadata;
  metadata.width = 1080;
  metadata.height = 1920;
  metadata.stride = 4320;
  CHECK(validate_frame_metadata(metadata).is_ok());

  FrameMetadata zero_width = metadata;
  zero_width.width = 0;
  CHECK(validate_frame_metadata(zero_width).is_error());

  FrameMetadata short_stride = metadata;
  short_stride.stride = 4000;  // < 1080*4
  const Status short_status = validate_frame_metadata(short_stride);
  REQUIRE(short_status.is_error());
  CHECK_EQ(short_status.error().code, std::string("android.invalid_frame_metadata"));

  FrameMetadata bad_rotation = metadata;
  bad_rotation.rotation_degrees = 45;
  CHECK(validate_frame_metadata(bad_rotation).is_error());

  FrameMetadata negative_time = metadata;
  negative_time.capture_time_ns = -1;
  CHECK(validate_frame_metadata(negative_time).is_error());

  FrameMetadata valid_rotation = metadata;
  valid_rotation.rotation_degrees = 270;
  CHECK(validate_frame_metadata(valid_rotation).is_ok());
}

BONK_TEST(frame_source, rendered_metadata_is_stable_and_greppable) {
  FrameMetadata metadata;
  metadata.width = 1080;
  metadata.height = 1920;
  metadata.stride = 4320;
  metadata.sequence = 3;

  CHECK_EQ(render_frame_metadata(metadata),
           std::string("frame source=still-snapshot format=rgba8 1080x1920 stride=4320 rotation=unknown seq=3"));

  metadata.rotation_degrees = 90;
  CHECK_EQ(render_frame_metadata(metadata),
           std::string("frame source=still-snapshot format=rgba8 1080x1920 stride=4320 rotation=90 seq=3"));
}

BONK_TEST(frame_source, source_names_are_stable) {
  CHECK_EQ(to_string(FrameSourceKind::StillSnapshot), std::string_view("still-snapshot"));
  CHECK_EQ(to_string(FrameSourceKind::Streaming), std::string_view("streaming"));
  CHECK_EQ(to_string(FramePixelFormat::Rgba8), std::string_view("rgba8"));
}
