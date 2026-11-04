#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "bonk/crypto/crypto.hpp"

namespace bonk::crypto {

Bytes random_bytes(std::size_t count) {
  Bytes buffer(count, 0);
  if (count == 0) return buffer;

  std::size_t filled = 0;
  while (filled < count) {
    const ssize_t result = ::getrandom(buffer.data() + filled, count - filled, 0);
    if (result > 0) {
      filled += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) continue;
    break;  // fall back to /dev/urandom
  }
  if (filled == count) return buffer;

  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    std::size_t got = 0;
    while (got < count) {
      const ssize_t result = ::read(fd, buffer.data() + got, count - got);
      if (result <= 0) break;
      got += static_cast<std::size_t>(result);
    }
    ::close(fd);
    if (got == count) return buffer;
  }
  // Last resort: this must never happen, but never return uninitialised memory.
  std::fprintf(stderr, "bonk: warning: no OS randomness available; output is predictable\n");
  for (std::size_t i = 0; i < count; ++i) buffer[i] = static_cast<std::uint8_t>(i * 37 + 11);
  return buffer;
}

void constant_time_copy(void* destination, const void* source, std::size_t length) {
  volatile std::uint8_t* out = static_cast<volatile std::uint8_t*>(destination);
  const auto* in = static_cast<const std::uint8_t*>(source);
  for (std::size_t i = 0; i < length; ++i) out[i] = in[i];
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference = static_cast<std::uint8_t>(difference | (static_cast<std::uint8_t>(a[i]) ^ static_cast<std::uint8_t>(b[i])));
  }
  return difference == 0;
}

std::string to_hex(std::string_view bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<std::uint8_t>(byte);
    out.push_back(kDigits[value >> 4]);
    out.push_back(kDigits[value & 0x0F]);
  }
  return out;
}

Bytes from_hex(std::string_view hex) {
  Bytes out;
  out.reserve(hex.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int high = nibble(hex[i]);
    const int low = nibble(hex[i + 1]);
    if (high < 0 || low < 0) break;
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return out;
}

}  // namespace bonk::crypto
