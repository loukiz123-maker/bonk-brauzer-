// LynxDroid frame source.
//
// A single `adb exec-out screencap -p` is a STILL FRAME, not a video stream.
// This module makes that distinction explicit in the data it returns:
//
//   * `FrameSourceKind::StillSnapshot` is the only implemented source;
//   * every frame carries sequence + monotonic timestamp + geometry so a
//     caller can detect staleness and correlate a frame with an input action;
//   * rotation is `std::optional` because a screenshot PNG does not report it.
//     Guessing 0 would silently lie to a caller mapping taps to pixels.
//
// Nothing here claims low latency or continuous capture.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "bonk/android/adb_client.hpp"
#include "bonk/core/result.hpp"
#include "bonk/image/image.hpp"

namespace bonk::android {

enum class FrameSourceKind { StillSnapshot, Streaming };

[[nodiscard]] std::string_view to_string(FrameSourceKind kind) noexcept;

enum class FramePixelFormat { Rgba8 };

[[nodiscard]] std::string_view to_string(FramePixelFormat format) noexcept;

/// Geometry and provenance of one captured frame. All fields are observations,
/// except `sequence` (caller-assigned) and `rotation_degrees` (may be unknown).
struct FrameMetadata {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Bytes per row of the decoded buffer; width*4 for RGBA8.
  std::uint32_t stride = 0;
  FramePixelFormat format = FramePixelFormat::Rgba8;
  FrameSourceKind source = FrameSourceKind::StillSnapshot;
  /// nullopt when the capture path cannot report orientation. Never default 0.
  std::optional<std::uint16_t> rotation_degrees;
  /// Monotonic timestamp of capture, nanoseconds since an arbitrary epoch.
  /// Only differences are meaningful; it is not a wall-clock time.
  std::int64_t capture_time_ns = 0;
  /// Caller-assigned frame counter, used to correlate frames and inputs.
  std::uint64_t sequence = 0;
};

struct StillFrame {
  image::Image image;
  FrameMetadata metadata;
};

/// Validates metadata invariants (non-zero geometry, stride >= width*4,
/// RGBA8 only, non-negative timestamp).
[[nodiscard]] Status validate_frame_metadata(const FrameMetadata& metadata);

/// Captures and decodes exactly one still frame, returning it with metadata
/// describing what was actually observed. Requires ADB server consent.
///
/// `sequence` must be greater than `previous_sequence` when the caller has one,
/// so a stall shows up as a non-increasing counter rather than a silent repeat.
[[nodiscard]] Result<StillFrame> capture_still_frame(
    const AdbConfig& config, std::uint64_t sequence,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Human-readable, greppable one-line summary:
///   frame source=still-snapshot format=rgba8 1080x1920 stride=4320 rotation=unknown seq=1
[[nodiscard]] std::string render_frame_metadata(const FrameMetadata& metadata);

}  // namespace bonk::android
