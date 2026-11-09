#include "bonk/android/preflight.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace bonk::android {
namespace {

using Path = std::filesystem::path;

PathProbe probe(ProbeState state, Path path, std::string message) {
  return PathProbe{state, std::move(path), std::move(message)};
}

bool has_embedded_nul(const Path& path) { return path.native().find('\0') != Path::string_type::npos; }

PathProbe probe_file(const Path& path, bool executable) {
  if (path.empty()) return probe(ProbeState::NotConfigured, path, "path was not selected");
  if (has_embedded_nul(path)) return probe(ProbeState::Invalid, path, "path contains an embedded NUL byte");
  if (!path.is_absolute()) return probe(ProbeState::Invalid, path, "an absolute path must be selected explicitly");

  std::error_code error;
  const std::filesystem::file_status file = std::filesystem::status(path, error);
  if (error) {
    const ProbeState state = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory
                                 ? ProbeState::Missing
                                 : ProbeState::Inaccessible;
    return probe(state, path, "cannot inspect file: " + error.message());
  }
  if (!std::filesystem::exists(file)) return probe(ProbeState::Missing, path, "file does not exist");
  if (!std::filesystem::is_regular_file(file)) return probe(ProbeState::Invalid, path, "path is not a regular file");

  const int mode = executable ? R_OK | X_OK : R_OK;
  if (::access(path.c_str(), mode) != 0) {
    const int error_number = errno;
    return probe(ProbeState::Inaccessible, path, std::string("file is not accessible: ") + std::strerror(error_number));
  }
  return probe(ProbeState::Available, path, executable ? "regular executable file is present and accessible"
                                                        : "regular file is present and readable");
}

PathProbe probe_directory(const Path& path) {
  if (path.empty()) return probe(ProbeState::NotConfigured, path, "directory was not selected");
  if (has_embedded_nul(path)) return probe(ProbeState::Invalid, path, "path contains an embedded NUL byte");
  if (!path.is_absolute()) return probe(ProbeState::Invalid, path, "an absolute path must be selected explicitly");

  std::error_code error;
  const std::filesystem::file_status file = std::filesystem::status(path, error);
  if (error) {
    const ProbeState state = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory
                                 ? ProbeState::Missing
                                 : ProbeState::Inaccessible;
    return probe(state, path, "cannot inspect directory: " + error.message());
  }
  if (!std::filesystem::exists(file)) return probe(ProbeState::Missing, path, "directory does not exist");
  if (!std::filesystem::is_directory(file)) return probe(ProbeState::Invalid, path, "path is not a directory");

  if (::access(path.c_str(), R_OK | X_OK) != 0) {
    const int error_number = errno;
    return probe(ProbeState::Inaccessible, path,
                 std::string("directory is not accessible: ") + std::strerror(error_number));
  }
  return probe(ProbeState::Available, path, "directory is present and accessible");
}

PathProbe probe_kvm(const Path& path) {
  if (path.empty()) return probe(ProbeState::NotConfigured, path, "KVM device path was not selected");
  if (has_embedded_nul(path)) return probe(ProbeState::Invalid, path, "path contains an embedded NUL byte");
  if (!path.is_absolute()) return probe(ProbeState::Invalid, path, "KVM device path must be absolute");

  std::error_code error;
  const std::filesystem::file_status file = std::filesystem::status(path, error);
  if (error) {
    const ProbeState state = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory
                                 ? ProbeState::Missing
                                 : ProbeState::Inaccessible;
    return probe(state, path, "cannot inspect KVM device: " + error.message());
  }
  if (!std::filesystem::exists(file)) return probe(ProbeState::Missing, path, "KVM device does not exist");
  if (!std::filesystem::is_character_file(file)) {
    return probe(ProbeState::Invalid, path, "path is not a character device");
  }
  if (::access(path.c_str(), R_OK | W_OK) != 0) {
    const int error_number = errno;
    return probe(ProbeState::Inaccessible, path,
                 std::string("KVM device is not readable/writable by this user: ") + std::strerror(error_number));
  }
  return probe(ProbeState::Available, path,
               "KVM device is readable and writable; virtualization capability was not fully tested");
}

bool valid_avd_name(std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.front() == '-') return false;
  for (const unsigned char character : name) {
    if (character == '/' || character == '\\' || std::iscntrl(character) != 0) return false;
  }
  return true;
}

std::string_view trim_ascii(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.remove_suffix(1);
  return value;
}

struct IniValue {
  bool ok = false;
  std::string value;
  std::string message;
};

IniValue read_ini_value(const Path& path, std::string_view requested_key) {
  std::ifstream input(path);
  if (!input) return {false, {}, "cannot read INI file"};

  std::string line;
  std::string found_value;
  bool found = false;
  while (std::getline(input, line)) {
    const std::string_view view = trim_ascii(line);
    if (view.empty() || view.front() == '#' || view.front() == ';') continue;

    const std::size_t separator = view.find('=');
    if (separator == std::string_view::npos) continue;
    const std::string_view key = trim_ascii(view.substr(0, separator));
    if (key != requested_key) continue;
    if (found) return {false, {}, "INI contains a duplicate '" + std::string(requested_key) + "' key"};

    const std::string_view value = trim_ascii(view.substr(separator + 1));
    if (value.empty()) return {false, {}, "INI key '" + std::string(requested_key) + "' has an empty value"};
    found_value.assign(value);
    found = true;
  }
  if (input.bad()) return {false, {}, "I/O error while reading INI file"};
  if (!found) return {false, {}, "INI key '" + std::string(requested_key) + "' was not found"};
  if (found_value.find('\0') != std::string::npos) return {false, {}, "INI value contains an embedded NUL byte"};
  return {true, std::move(found_value), {}};
}

bool path_is_within(const Path& root, const Path& candidate) {
  const Path normalized_root = root.lexically_normal();
  const Path normalized_candidate = candidate.lexically_normal();
  const Path relative = normalized_candidate.lexically_relative(normalized_root);
  if (relative.empty()) return normalized_candidate == normalized_root;
  return *relative.begin() != "..";
}

PathProbe dependent_probe(std::string message) {
  return probe(ProbeState::NotConfigured, {}, std::move(message));
}

}  // namespace

std::string_view to_string(ProbeState state) noexcept {
  switch (state) {
    case ProbeState::Available:
      return "available";
    case ProbeState::NotConfigured:
      return "not-configured";
    case ProbeState::Missing:
      return "missing";
    case ProbeState::Invalid:
      return "invalid";
    case ProbeState::Inaccessible:
      return "inaccessible";
  }
  return "invalid";
}

bool RuntimePreflightReport::static_inputs_present() const noexcept {
  return emulator.available() && android_sdk_root.available() && avd_home.available() && avd_definition.available() &&
         avd_data.available() && avd_config.available() && system_image.available();
}

RuntimePreflightReport preflight_runtime(const RuntimePreflightRequest& request) {
  RuntimePreflightReport report;
  report.emulator = probe_file(request.emulator_executable, true);
  report.adb = probe_file(request.adb_executable, true);
  report.android_sdk_root = probe_directory(request.android_sdk_root);
  report.avd_home = probe_directory(request.avd_home);
  report.kvm_device = probe_kvm(request.kvm_device);

  if (!report.avd_home.available()) {
    report.avd_definition = dependent_probe("cannot inspect an AVD definition until AVD home is available");
    report.avd_data = dependent_probe("cannot inspect AVD data until AVD home is available");
    report.avd_config = dependent_probe("cannot inspect AVD config until AVD home is available");
    report.system_image = dependent_probe("cannot inspect system image until AVD config is available");
    return report;
  }
  if (!valid_avd_name(request.avd_name)) {
    report.avd_definition = probe(ProbeState::Invalid, request.avd_home,
                                  "AVD name is empty, a path, a command option, or contains control characters");
    report.avd_data = dependent_probe("cannot resolve AVD data from an invalid AVD name");
    report.avd_config = dependent_probe("cannot inspect AVD config from an invalid AVD name");
    report.system_image = dependent_probe("cannot inspect system image from an invalid AVD name");
    return report;
  }

  const Path avd_definition_path = request.avd_home / (request.avd_name + ".ini");
  report.avd_definition = probe_file(avd_definition_path, false);
  if (!report.avd_definition.available()) {
    report.avd_data = dependent_probe("cannot resolve AVD data until its definition file is readable");
    report.avd_config = dependent_probe("cannot inspect AVD config until AVD data is resolved");
    report.system_image = dependent_probe("cannot inspect system image until AVD config is available");
    return report;
  }

  const IniValue avd_path = read_ini_value(avd_definition_path, "path");
  if (!avd_path.ok) {
    report.avd_data = probe(ProbeState::Invalid, avd_definition_path, avd_path.message);
    report.avd_config = dependent_probe("cannot inspect AVD config until AVD data is resolved");
    report.system_image = dependent_probe("cannot inspect system image until AVD config is available");
    return report;
  }

  const Path avd_data_path(avd_path.value);
  if (!avd_data_path.is_absolute()) {
    report.avd_data = probe(ProbeState::Invalid, avd_data_path, "AVD definition path must be absolute");
    report.avd_config = dependent_probe("cannot inspect AVD config until AVD data is resolved");
    report.system_image = dependent_probe("cannot inspect system image until AVD config is available");
    return report;
  }
  report.avd_data = probe_directory(avd_data_path);
  if (!report.avd_data.available()) {
    report.avd_config = dependent_probe("cannot inspect AVD config until AVD data directory is available");
    report.system_image = dependent_probe("cannot inspect system image until AVD config is available");
    return report;
  }

  const Path avd_config_path = avd_data_path / "config.ini";
  report.avd_config = probe_file(avd_config_path, false);
  if (!report.avd_config.available()) {
    report.system_image = dependent_probe("cannot resolve system image until AVD config is readable");
    return report;
  }

  const IniValue image_directory_value = read_ini_value(avd_config_path, "image.sysdir.1");
  if (!image_directory_value.ok) {
    report.system_image = probe(ProbeState::Invalid, avd_config_path, image_directory_value.message);
    return report;
  }
  if (!report.android_sdk_root.available()) {
    report.system_image = dependent_probe("cannot resolve a relative system image until SDK root is available");
    return report;
  }

  const Path image_directory_from_config(image_directory_value.value);
  const Path image_directory = image_directory_from_config.is_absolute()
                                   ? image_directory_from_config.lexically_normal()
                                   : (request.android_sdk_root / image_directory_from_config).lexically_normal();
  if (!path_is_within(request.android_sdk_root, image_directory)) {
    report.system_image = probe(ProbeState::Invalid, image_directory,
                                "system image path is outside the selected SDK root; external image provenance needs review");
    return report;
  }

  std::error_code error;
  const Path canonical_sdk_root = std::filesystem::canonical(request.android_sdk_root, error);
  if (error) {
    report.system_image = probe(ProbeState::Inaccessible, request.android_sdk_root,
                                "cannot resolve SDK root: " + error.message());
    return report;
  }
  const Path canonical_image_directory = std::filesystem::canonical(image_directory, error);
  if (error) {
    const ProbeState state = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory
                                 ? ProbeState::Missing
                                 : ProbeState::Inaccessible;
    report.system_image = probe(state, image_directory, "cannot resolve system image directory: " + error.message());
    return report;
  }
  if (!path_is_within(canonical_sdk_root, canonical_image_directory)) {
    report.system_image = probe(ProbeState::Invalid, canonical_image_directory,
                                "system image resolves outside the selected SDK root; external image provenance needs review");
    return report;
  }

  report.system_image = probe_file(canonical_image_directory / "system.img", false);
  if (report.system_image.available()) {
    report.system_image.message =
        "system.img is present and readable; image format, origin, and bootability were not checked";
  }
  return report;
}

}  // namespace bonk::android
