#include "bonk/core/version.hpp"

namespace bonk {

std::string version_string() {
  std::string out;
  out += std::to_string(kVersionMajor);
  out += '.';
  out += std::to_string(kVersionMinor);
  out += '.';
  out += std::to_string(kVersionPatch);
  out += '-';
  out += kVersionPhase;
  return out;
}

std::string build_string() {
  std::string out = "compiler: ";
#if defined(__clang__)
  out += "clang++ ";
  out += __clang_version__;
#elif defined(__GNUC__)
  out += "g++ ";
  out += __VERSION__;
#elif defined(_MSC_VER)
  out += "msvc ";
  out += std::to_string(_MSC_VER);
#else
  out += "unknown";
#endif

#if defined(__GLIBCXX__)
  out += " / libstdc++";
#elif defined(_LIBCPP_VERSION)
  out += " / libc++";
#endif

  out += " / c++";
  out += std::to_string(__cplusplus / 100 % 100);
  out += " / ";

#if defined(NDEBUG)
  out += "release";
#else
  out += "debug";
#endif
  return out;
}

}  // namespace bonk
