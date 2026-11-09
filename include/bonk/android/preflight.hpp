// Non-destructive checks for explicitly selected Android SDK / AVD paths.
// These checks inspect files only; they do not run an emulator or validate that
// a guest image can boot.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bonk::android {

enum class ProbeState { Available, NotConfigured, Missing, Invalid, Inaccessible };

[[nodiscard]] std::string_view to_string(ProbeState state) noexcept;

struct PathProbe {
  ProbeState state = ProbeState::NotConfigured;
  std::filesystem::path path;
  std::string message;

  [[nodiscard]] bool available() const noexcept { return state == ProbeState::Available; }
};

/// User-selected paths. The preflight does not search PATH, infer an SDK root,
/// choose an AVD, or alter permissions on any path.
struct RuntimePreflightRequest {
  std::filesystem::path emulator_executable;
  std::filesystem::path adb_executable;
  std::filesystem::path android_sdk_root;
  std::filesystem::path avd_home;
  std::string avd_name;
  std::filesystem::path kvm_device = "/dev/kvm";
};

/// File/path results only. `system_image` means readable system.img exists;
/// its format, provenance, and bootability are not verified here.
struct RuntimePreflightReport {
  PathProbe emulator;
  PathProbe adb;
  PathProbe android_sdk_root;
  PathProbe avd_home;
  PathProbe avd_definition;
  PathProbe avd_data;
  PathProbe avd_config;
  PathProbe system_image;
  PathProbe kvm_device;

  /// True only when the selected runtime and AVD-related files were found.
  /// This is not a prediction or proof that Android will boot.
  [[nodiscard]] bool static_inputs_present() const noexcept;
  [[nodiscard]] bool guest_control_tool_available() const noexcept { return adb.available(); }
  [[nodiscard]] bool kvm_device_accessible() const noexcept { return kvm_device.available(); }
};

/// Checks only the exact paths supplied by the caller. No subprocesses run and
/// no files are created, downloaded, or modified.
[[nodiscard]] RuntimePreflightReport preflight_runtime(const RuntimePreflightRequest& request);

}  // namespace bonk::android
