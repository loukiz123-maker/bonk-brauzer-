#include "bonk/android/frame_source.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace bonk::android {
namespace {

constexpr std::uint32_t kBytesPerPixelRgba8 = 4;

}  // namespace

std::string_view to_string(FrameSourceKind kind) noexcept {
  switch (kind) {
    case FrameSourceKind::StillSnapshot:
      return "still-snapshot";
    case FrameSourceKind::Streaming:
      return "streaming";
  }
  return "still-snapshot";
}

std::string_view to_string(FramePixelFormat format) noexcept {
  switch (format) {
    case FramePixelFormat::Rgba8:
      return "rgba8";
  }
  return "rgba8";
}

Status validate_frame_metadata(const FrameMetadata& metadata) {
  if (metadata.width == 0 || metadata.height == 0) {
    return Status::failure(
        Error::make("android.invalid_frame_metadata", "frame geometry must be non-zero"));
  }
  // Guard the stride computation before multiplying.
  const std::uint64_t minimum_stride =
      static_cast<std::uint64_t>(metadata.width) * kBytesPerPixelRgba8;
  if (minimum_stride > 0xFFFFFFFFull) {
    return Status::failure(
        Error::make("android.invalid_frame_metadata", "frame width is too large"));
  }
  if (metadata.stride < minimum_stride) {
    return Status::failure(Error::make(
        "android.invalid_frame_metadata", "frame stride is smaller than one RGBA8 row"));
  }
  if (metadata.rotation_degrees.has_value() && *metadata.rotation_degrees % 90 != 0) {
    return Status::failure(
        Error::make("android.invalid_frame_metadata", "frame rotation must be a multiple of 90 degrees"));
  }
  if (metadata.capture_time_ns < 0) {
    return Status::failure(
        Error::make("android.invalid_frame_metadata", "frame timestamp must not be negative"));
  }
  return Status::success();
}

Result<StillFrame> capture_still_frame(const AdbConfig& config, std::uint64_t sequence,
                                      std::chrono::milliseconds command_timeout) {
  const Result<image::Image> captured = capture_screenshot_rgba(config, command_timeout);
  if (captured.is_error()) return Result<StillFrame>::failure(captured.error());
  if (captured.value().empty()) {
    return Result<StillFrame>::failure(
        Error::make("android.empty_frame", "screenshot decoded to an empty image"));
  }

  StillFrame frame;
  frame.image = std::move(captured.value());
  frame.metadata.width = static_cast<std::uint32_t>(frame.image.width);
  frame.metadata.height = static_cast<std::uint32_t>(frame.image.height);
  frame.metadata.stride = frame.metadata.width * kBytesPerPixelRgba8;
  frame.metadata.format = FramePixelFormat::Rgba8;
  frame.metadata.source = FrameSourceKind::StillSnapshot;
  // A screencap PNG carries no orientation, so report it as unknown rather
  // than defaulting to 0.
  frame.metadata.rotation_degrees = std::nullopt;
  frame.metadata.capture_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
  frame.metadata.sequence = sequence;

  const Status valid = validate_frame_metadata(frame.metadata);
  if (valid.is_error()) return Result<StillFrame>::failure(valid.error());
  return Result<StillFrame>::success(std::move(frame));
}

std::string render_frame_metadata(const FrameMetadata& metadata) {
  std::string out = "frame source=";
  out += to_string(metadata.source);
  out += " format=";
  out += to_string(metadata.format);
  out += ' ';
  out += std::to_string(metadata.width);
  out += 'x';
  out += std::to_string(metadata.height);
  out += " stride=";
  out += std::to_string(metadata.stride);
  out += " rotation=";
  if (metadata.rotation_degrees.has_value()) {
    out += std::to_string(*metadata.rotation_degrees);
  } else {
    out += "unknown";
  }
  out += " seq=";
  out += std::to_string(metadata.sequence);
  return out;
}

}  // namespace bonk::android
