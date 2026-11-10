// QA fuzzer for the host-side Android engine parsers (not part of the build).
//
// Mutates a valid PNG (re-computing chunk CRCs so mutations reach the decoder),
// feeds random text to the ADB output parsers and probes the edges of
// normalized_touch_to_pixels. Run it under ASan/UBSan from the repository root:
//
//   make -j2 SAN=address,undefined MODE=debug
//   g++ -std=c++20 -Iinclude -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       qa/tests/engine_parser_fuzz.cpp \
//       $(find 'build-address,undefined/obj/src' -name '*.o' ! -name main.o) -o /tmp/enginefuzz
//   /tmp/enginefuzz tests/fixtures/logo.png 30000
//
// On the engine branch (arena/906ff810-bonk-brauzer) add -DQA_ENGINE_BRANCH_API
// to also cover the device-list and package-path parsers added there.
//
// It proves the parsers do not crash or read out of bounds on hostile input.
// It says nothing about a real Android device: none is involved.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include "bonk/android/adb_client.hpp"
using namespace bonk;
static std::uint32_t crc(const std::string& d, size_t off, size_t n) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) { c ^= static_cast<std::uint8_t>(d[off + i]); for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
  return c ^ 0xFFFFFFFFu;
}
static void fix_crcs(std::string& png) {
  size_t pos = 8;
  while (pos + 12 <= png.size()) {
    std::uint32_t len = (std::uint8_t(png[pos]) << 24) | (std::uint8_t(png[pos+1]) << 16) | (std::uint8_t(png[pos+2]) << 8) | std::uint8_t(png[pos+3]);
    if (len > png.size() - pos - 12) break;
    std::uint32_t c = crc(png, pos + 4, 4 + len);
    size_t co = pos + 8 + len;
    png[co] = char(c >> 24); png[co+1] = char(c >> 16); png[co+2] = char(c >> 8); png[co+3] = char(c);
    pos = co + 4;
  }
}
int main(int argc, char** argv) {
  std::ifstream in(argv[1], std::ios::binary); std::stringstream ss; ss << in.rdbuf(); const std::string seed = ss.str();
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 20000;
  std::mt19937 rng(12345);
  long ok = 0, bad = 0;
  { auto r = android::decode_screenshot_png(seed); std::printf("seed decode: %s\n", r.is_ok() ? "ok" : r.error().message.c_str()); }
  for (int i = 0; i < iterations; ++i) {
    std::string m = seed;
    int edits = 1 + rng() % 6;
    for (int e = 0; e < edits; ++e) {
      switch (rng() % 5) {
        case 0: m[rng() % m.size()] = char(rng()); break;
        case 1: m.resize(rng() % (m.size() + 1)); if (m.empty()) m = "x"; break;
        case 2: { size_t p = 8 + rng() % 30; if (p < m.size()) m[p] = char(rng()); } break;   // IHDR area
        case 3: m.insert(rng() % m.size(), std::string(rng() % 9, char(rng()))); break;
        case 4: { size_t p = rng() % m.size(); m[p] = char(m[p] ^ (1 << (rng() % 8))); } break;
      }
    }
    if (rng() % 4 != 0) fix_crcs(m);
    auto r = android::decode_screenshot_png(m);
    if (r.is_ok()) { ++ok; const auto& img = r.value(); if (img.rgba.size() != size_t(img.width) * img.height * 4) { std::printf("SIZE MISMATCH\n"); return 1; } } else ++bad;
  }
  std::printf("png fuzz: %ld decoded, %ld rejected\n", ok, bad);
  // text parsers
  const std::string alphabet = "abcdeviceofflineunauthorized \t\n\r:._-0123456789packageErrorFailureStatus=/\0\xff";
  for (int i = 0; i < 200000; ++i) {
    std::string s; int n = rng() % 80; for (int k = 0; k < n; ++k) s.push_back(alphabet[rng() % (alphabet.size())]);
    (void)android::parse_adb_devices_output("emulator-5554", s);
#ifdef QA_ENGINE_BRANCH_API  // parsers that exist only on the engine branch so far
    (void)android::parse_adb_devices_list_output(s);
    (void)android::parse_adb_package_paths_output(s);
#endif
    (void)android::parse_boot_completed_property(s);
  }
  const float specials[] = {0.f, 1.f, -0.f, 0.5f, std::nanf(""), std::numeric_limits<float>::infinity(), -1.f, 1.0000001f, std::numeric_limits<float>::denorm_min(), 0.99999994f};
  for (std::uint32_t w : {0u, 1u, 2u, 1080u, 16384u, 16385u, 4294967295u}) for (std::uint32_t h : {0u, 1u, 1920u, 16384u, 4294967295u}) for (float x : specials) for (float y : specials) {
    auto r = android::normalized_touch_to_pixels(w, h, x, y);
    if (r.is_ok() && (r.value().x >= w || r.value().y >= h)) { std::printf("OUT OF RANGE %u %u %f %f -> %u %u\n", w, h, x, y, r.value().x, r.value().y); return 1; }
  }
  std::printf("text/coordinate fuzz: done\n");
}
