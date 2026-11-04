// Bonk Brauzer — version and build identification.
//
// Kept in a dedicated header so that every module (and the CLI, and tests) can
// report the same version string without pulling in heavier dependencies.
#pragma once

#include <string>
#include <string_view>

namespace bonk {

inline constexpr std::string_view kProjectName = "Bonk Brauzer";
inline constexpr std::string_view kProjectSlug = "bonk";
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

// Human readable milestone, bumped at the end of every stage.
// Stage 0 = project skeleton, no rendering yet.
inline constexpr std::string_view kVersionPhase = "stage-0";

// "0.1.0-stage-0"
std::string version_string();

// Compiler / standard library / build mode, e.g.
// "g++ 12.2.0 / libstdc++ / c++20 / release". Used by `bonk version --full`
// and by bug reports; must stay deterministic for a given toolchain so that
// it is safe to assert on the format in tests.
std::string build_string();

}  // namespace bonk
