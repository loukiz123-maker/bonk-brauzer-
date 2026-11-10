// Runtime-neutral host observation shared by the engine state model and the
// launcher plan.
//
// No runtime backend is selected in this tree: the Waydroid adapter was removed
// on the project owner's decision (LynxDroid must not depend on Waydroid).
// Whatever backend is chosen next reports its host-side state through these
// types; nothing here spawns a process or names a specific runtime.
#pragma once

#include <string_view>

namespace bonk::android {

enum class RuntimeState { Unknown, Stopped, Running };

[[nodiscard]] std::string_view to_string(RuntimeState state) noexcept;

/// Host-side state of a runtime backend.
///
/// `session` is the user-facing runtime instance the launcher starts and
/// stops. `container` is the shared host service, container or VM behind it;
/// a backend without such a second layer leaves it Unknown.
struct RuntimeStatus {
  RuntimeState session = RuntimeState::Unknown;
  RuntimeState container = RuntimeState::Unknown;
};

}  // namespace bonk::android
