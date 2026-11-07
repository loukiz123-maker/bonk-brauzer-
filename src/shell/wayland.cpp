#include "bonk/shell/wayland.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace bonk::shell::wayland {
namespace proto {

void append_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

namespace {

void pad_to_4(std::string& out) {
  while (out.size() % 4 != 0) out.push_back('\0');
}

std::uint32_t read_u32_at(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) << 24);
}

}  // namespace

std::string string_arg(std::string_view text) {
  std::string out;
  append_u32(out, static_cast<std::uint32_t>(text.size() + 1));  // the trailing NUL counts
  out.append(text);
  out.push_back('\0');
  pad_to_4(out);
  return out;
}

std::string array_arg(std::string_view bytes) {
  std::string out;
  append_u32(out, static_cast<std::uint32_t>(bytes.size()));
  out.append(bytes);
  pad_to_4(out);
  return out;
}

std::string message(std::uint32_t object, std::uint16_t opcode, std::string_view body) {
  std::string out;
  append_u32(out, object);
  const auto size = static_cast<std::uint32_t>(8 + body.size());
  append_u32(out, (size << 16) | opcode);
  out.append(body);
  return out;
}

std::string bind_message(std::uint32_t registry, std::uint32_t name, std::string_view interface, std::uint32_t version,
                         std::uint32_t id) {
  std::string body;
  append_u32(body, name);
  body.append(string_arg(interface));
  append_u32(body, version);
  append_u32(body, id);
  return message(registry, kRegistryBind, body);
}

std::string sync_message(std::uint32_t callback) {
  std::string body;
  append_u32(body, callback);
  return message(kDisplayId, kDisplaySync, body);
}

std::string create_pool_message(std::uint32_t pool, std::uint32_t size) {
  // The file descriptor travels in an SCM_RIGHTS control message, not in the body.
  std::string body;
  append_u32(body, pool);
  append_u32(body, size);
  return message(1, kShmCreatePool, body);
}

std::string create_buffer_message(std::uint32_t pool, std::uint32_t buffer, std::uint32_t offset, int width,
                                  int height, std::uint32_t stride, std::uint32_t format) {
  std::string body;
  append_u32(body, buffer);
  append_u32(body, offset);
  append_u32(body, static_cast<std::uint32_t>(width));
  append_u32(body, static_cast<std::uint32_t>(height));
  append_u32(body, stride);
  append_u32(body, format);
  return message(pool, kShmPoolCreateBuffer, body);
}

bool decode_header(std::string_view bytes, std::size_t offset, Header& header) {
  if (offset + 8 > bytes.size()) return false;
  header.object = read_u32_at(bytes, offset);
  const std::uint32_t second = read_u32_at(bytes, offset + 4);
  header.size = static_cast<std::uint16_t>(second >> 16);
  header.opcode = static_cast<std::uint16_t>(second & 0xFFFF);
  return true;
}

bool Reader::u32(std::uint32_t& value) {
  if (position_ + 4 > body_.size()) return false;
  value = read_u32_at(body_, position_);
  position_ += 4;
  return true;
}

bool Reader::i32(std::int32_t& value) {
  std::uint32_t raw = 0;
  if (!u32(raw)) return false;
  value = static_cast<std::int32_t>(raw);
  return true;
}

bool Reader::string(std::string& value) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (position_ + length > body_.size()) return false;
  // The length includes the terminating NUL, which we do not keep.
  const std::size_t text_length = length == 0 ? 0 : static_cast<std::size_t>(length - 1);
  value.assign(body_.substr(position_, text_length));
  position_ += length;
  position_ += (4 - (position_ % 4)) % 4;  // strings are padded to a 4-byte boundary
  return true;
}

bool Reader::fixed(int& value) {
  std::int32_t raw = 0;
  if (!i32(raw)) return false;
  value = static_cast<int>(raw / 256);
  return true;
}

bool decode_configure(std::string_view body, Configure& configure) {
  Reader reader(body);
  std::int32_t width = 0;
  std::int32_t height = 0;
  if (!reader.i32(width) || !reader.i32(height)) return false;
  configure.width = static_cast<int>(width);
  configure.height = static_cast<int>(height);
  return true;
}

std::uint32_t keysym_for_evdev(std::uint32_t code) {
  // evdev keycodes: https://www.kernel.org/doc/html/latest/input/event-codes.html
  switch (code) {
    case 1: return 0xFF1B;    // Escape
    case 14: return 0xFF08;   // BackSpace
    case 15: return 0xFF09;   // Tab
    case 28: return 0xFF0D;   // Return
    case 57: return 0x0020;   // space
    case 102: return 0xFF50;  // Home
    case 103: return 0xFF52;  // Up
    case 104: return 0xFF55;  // Page_Up
    case 105: return 0xFF51;  // Left
    case 106: return 0xFF53;  // Right
    case 107: return 0xFF57;  // End
    case 108: return 0xFF54;  // Down
    case 109: return 0xFF56;  // Page_Down
    case 110: return 0xFF63;  // Insert
    case 111: return 0xFFFF;  // Delete
    default: break;
  }
  // US layout: digits, then the three letter rows. Keysyms for ASCII are the codepoints.
  static constexpr char kDigits[] = "1234567890";
  if (code >= 2 && code <= 11) return static_cast<std::uint32_t>(kDigits[code - 2]);
  static constexpr char kRow1[] = "qwertyuiop";
  if (code >= 16 && code <= 25) return static_cast<std::uint32_t>(kRow1[code - 16]);
  static constexpr char kRow2[] = "asdfghjkl";
  if (code >= 30 && code <= 38) return static_cast<std::uint32_t>(kRow2[code - 30]);
  static constexpr char kRow3[] = "zxcvbnm";
  if (code >= 44 && code <= 50) return static_cast<std::uint32_t>(kRow3[code - 44]);
  return 0;
}

}  // namespace proto

namespace {

Error wayland_error(std::string_view message) { return Error::make("shell.wayland", message); }

std::string runtime_dir() {
  const char* env = std::getenv("XDG_RUNTIME_DIR");
  return env != nullptr && *env != '\0' ? std::string(env) : std::string();
}

/// Creates a shareable file of `size` bytes: `memfd_create` where the kernel has it,
/// otherwise an unlinked temporary file in the runtime directory.
int create_shm_file(std::size_t size) {
#if defined(__linux__) && defined(SYS_memfd_create)
  const int fd = static_cast<int>(::syscall(SYS_memfd_create, "bonk-wayland", MFD_CLOEXEC));
  if (fd >= 0) {
    if (::ftruncate(fd, static_cast<off_t>(size)) == 0) return fd;
    ::close(fd);
  }
#endif
  std::string directory = runtime_dir();
  if (directory.empty()) directory = "/tmp";
  std::string path = directory + "/bonk-shm-XXXXXX";
  std::vector<char> writable(path.begin(), path.end());
  writable.push_back('\0');
  const int temporary = ::mkstemp(writable.data());
  if (temporary < 0) return -1;
  ::unlink(writable.data());  // the compositor holds its own reference
  if (::ftruncate(temporary, static_cast<off_t>(size)) != 0) {
    ::close(temporary);
    return -1;
  }
  return temporary;
}

}  // namespace

std::string socket_path(std::string_view display_name) {
  if (!display_name.empty()) {
    if (display_name.front() == '/') return std::string(display_name);  // absolute: used by tests
    const std::string directory = runtime_dir();
    if (directory.empty()) return {};
    return directory + "/" + std::string(display_name);
  }
  const char* env = std::getenv("WAYLAND_DISPLAY");
  if (env != nullptr && *env != '\0') {
    if (*env == '/') return std::string(env);
    const std::string directory = runtime_dir();
    if (directory.empty()) return {};
    return directory + "/" + env;
  }
  const std::string directory = runtime_dir();
  if (directory.empty()) return {};
  return directory + "/wayland-0";
}

bool display_available() {
  const std::string path = socket_path();
  if (path.empty()) return false;
  return ::access(path.c_str(), F_OK) == 0;
}

Window::~Window() { close(); }

Result<std::unique_ptr<Window>> Window::open(int width, int height, std::string_view title,
                                             std::string_view display_name) {
  std::unique_ptr<Window> window(new Window());
  window->width_ = width;
  window->height_ = height;

  Status status = window->connect_display(display_name);
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  status = window->bind_globals();
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  status = window->create_surface(title);
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  return Result<std::unique_ptr<Window>>::success(std::move(window));
}

Status Window::connect_display(std::string_view display_name) {
  const std::string path = socket_path(display_name);
  if (path.empty()) return Status::failure(wayland_error("WAYLAND_DISPLAY is not set and XDG_RUNTIME_DIR is empty"));

  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) return Status::failure(wayland_error("cannot create a socket"));

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message = "cannot connect to " + path + ": " + std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return Status::failure(wayland_error(message));
  }
  return Status::success();
}

Status Window::bind_globals() {
  registry_ = next_id_++;
  std::string body;
  proto::append_u32(body, registry_);
  Status status = flush();
  if (status.is_error()) return status;
  outgoing_.append(proto::message(proto::kDisplayId, proto::kDisplayGetRegistry, body));
  status = roundtrip();
  if (status.is_error()) return status;
  if (compositor_ == 0) return Status::failure(wayland_error("the compositor does not offer wl_compositor"));
  if (shm_ == 0) return Status::failure(wayland_error("the compositor does not offer wl_shm"));
  if (wm_base_ == 0) return Status::failure(wayland_error("the compositor does not offer xdg_wm_base"));
  return Status::success();
}

Status Window::create_surface(std::string_view title) {
  surface_ = next_id_++;
  std::string body;
  proto::append_u32(body, surface_);
  outgoing_.append(proto::message(compositor_, proto::kCompositorCreateSurface, body));

  xdg_surface_ = next_id_++;
  body.clear();
  proto::append_u32(body, xdg_surface_);
  proto::append_u32(body, surface_);
  outgoing_.append(proto::message(wm_base_, proto::kXdgWmBaseGetXdgSurface, body));

  xdg_toplevel_ = next_id_++;
  body.clear();
  proto::append_u32(body, xdg_toplevel_);
  outgoing_.append(proto::message(xdg_surface_, proto::kXdgSurfaceGetToplevel, body));

  // Ask for a server-side title bar where the compositor offers one (KWin, Mutter):
  // a `xdg_toplevel` without decorations has no close button at all. Best effort —
  // when the global is missing the window is simply undecorated.
  if (decoration_manager_ != 0) {
    decoration_ = next_id_++;
    body.clear();
    proto::append_u32(body, decoration_);
    proto::append_u32(body, xdg_toplevel_);
    outgoing_.append(proto::message(decoration_manager_, proto::kXdgDecorationGetToplevelDecoration, body));
    body.clear();
    proto::append_u32(body, proto::kXdgDecorationModeServerSide);
    outgoing_.append(proto::message(decoration_, proto::kXdgDecorationSetMode, body));
  }

  body.clear();
  body.append(proto::string_arg(title));
  outgoing_.append(proto::message(xdg_toplevel_, proto::kXdgToplevelSetTitle, body));

  body.clear();
  body.append(proto::string_arg("bonk"));
  outgoing_.append(proto::message(xdg_toplevel_, proto::kXdgToplevelSetAppId, body));

  // The first commit (without a buffer) asks the compositor for a configure event.
  outgoing_.append(proto::message(surface_, proto::kSurfaceCommit));

  Status status = flush();
  if (status.is_error()) return status;

  // Wait for the initial configure; the surface must not be painted before it arrives.
  for (int attempt = 0; attempt < 200 && !configured_; ++attempt) {
    status = dispatch(50);
    if (status.is_error()) return status;
  }
  if (!configured_) return Status::failure(wayland_error("the compositor did not configure the surface"));
  if (pending_width_ > 0 && pending_height_ > 0) {
    width_ = pending_width_;
    height_ = pending_height_;
  }
  return Status::success();
}

Status Window::roundtrip() {
  const std::uint32_t callback = next_id_++;
  sync_callback_ = callback;
  sync_done_ = false;
  Status status = flush();
  if (status.is_error()) return status;
  outgoing_.append(proto::sync_message(callback));
  for (int attempt = 0; attempt < 400 && !sync_done_; ++attempt) {
    status = dispatch(50);
    if (status.is_error()) return status;
  }
  if (!sync_done_) return Status::failure(wayland_error("the compositor did not answer wl_display.sync"));
  return Status::success();
}

Status Window::flush() {
  while (!outgoing_.empty()) {
    const ssize_t written = ::send(fd_, outgoing_.data(), outgoing_.size(), MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) continue;
      return Status::failure(wayland_error(std::string("write to the compositor failed: ") + std::strerror(errno)));
    }
    outgoing_.erase(0, static_cast<std::size_t>(written));
  }
  return Status::success();
}

Status Window::write_with_fd(std::string_view bytes, int fd) {
  Status status = flush();
  if (status.is_error()) return status;

  std::string_view rest = bytes;
  int descriptor = fd;
  while (!rest.empty()) {
    msghdr message{};
    iovec vector{};
    vector.iov_base = const_cast<char*>(rest.data());
    vector.iov_len = rest.size();
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))] = {};
    if (descriptor >= 0) {
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      cmsghdr* header = CMSG_FIRSTHDR(&message);
      header->cmsg_level = SOL_SOCKET;
      header->cmsg_type = SCM_RIGHTS;
      header->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(header), &descriptor, sizeof(int));
    }
    const ssize_t written = ::sendmsg(fd_, &message, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) continue;
      return Status::failure(wayland_error(std::string("cannot send a file descriptor: ") + std::strerror(errno)));
    }
    rest.remove_prefix(static_cast<std::size_t>(written));
    descriptor = -1;  // the descriptor rides along with the first byte only
  }
  return Status::success();
}

Status Window::dispatch(int timeout_ms) {
  Status status = flush();
  if (status.is_error()) return status;
  if (!display_error_.empty()) return Status::failure(wayland_error(display_error_));

  if (queue_.empty()) {
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) return Status::success();
      return Status::failure(wayland_error(std::string("poll failed: ") + std::strerror(errno)));
    }
    if (ready == 0) return Status::success();

    char chunk[8192];
    const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (received == 0) return Status::failure(wayland_error("the compositor closed the connection"));
    if (received < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return Status::success();
      return Status::failure(wayland_error(std::string("cannot read from the compositor: ") + std::strerror(errno)));
    }
    incoming_.append(chunk, static_cast<std::size_t>(received));
  }

  std::size_t offset = 0;
  while (true) {
    proto::Header header;
    if (!proto::decode_header(incoming_, offset, header)) break;
    if (!header.is_valid()) {
      incoming_.clear();
      return Status::failure(wayland_error("malformed message from the compositor"));
    }
    if (offset + header.size > incoming_.size()) break;  // the rest is still in flight
    handle_message(header, std::string_view(incoming_).substr(offset + 8, static_cast<std::size_t>(header.size) - 8));
    offset += header.size;
    if (!display_error_.empty()) break;
  }
  if (offset > 0) incoming_.erase(0, offset);
  if (!display_error_.empty()) return Status::failure(wayland_error(display_error_));
  return Status::success();
}

void Window::handle_message(const proto::Header& header, std::string_view body) {
  proto::Reader reader(body);

  if (header.object == proto::kDisplayId) {
    if (header.opcode == proto::kDisplayError) {
      std::uint32_t object = 0;
      std::uint32_t code = 0;
      std::string text;
      (void)reader.u32(object);
      (void)reader.u32(code);
      (void)reader.string(text);
      display_error_ = "protocol error " + std::to_string(code) + " on object " + std::to_string(object) + ": " + text;
    }
    return;
  }

  if (header.object == registry_) {
    if (header.opcode != proto::kRegistryGlobal) return;
    std::uint32_t name = 0;
    std::string interface;
    std::uint32_t version = 0;
    if (!reader.u32(name) || !reader.string(interface) || !reader.u32(version)) return;
    const std::uint32_t bind_version = std::min(version, proto::kVersion);
    if (interface == "wl_compositor" && compositor_ == 0) {
      compositor_ = next_id_++;
      outgoing_.append(proto::bind_message(registry_, name, interface, bind_version, compositor_));
    } else if (interface == "wl_shm" && shm_ == 0) {
      shm_ = next_id_++;
      outgoing_.append(proto::bind_message(registry_, name, interface, bind_version, shm_));
    } else if (interface == "xdg_wm_base" && wm_base_ == 0) {
      wm_base_ = next_id_++;
      outgoing_.append(proto::bind_message(registry_, name, interface, bind_version, wm_base_));
    } else if (interface == "zxdg_decoration_manager_v1" && decoration_manager_ == 0) {
      decoration_manager_ = next_id_++;
      outgoing_.append(proto::bind_message(registry_, name, interface, bind_version, decoration_manager_));
    } else if (interface == "wl_seat" && seat_ == 0) {
      seat_ = next_id_++;
      outgoing_.append(proto::bind_message(registry_, name, interface, bind_version, seat_));
    }
    return;
  }

  if (header.object == wm_base_ && header.opcode == proto::kXdgWmBasePing) {
    std::uint32_t serial = 0;
    if (!reader.u32(serial)) return;
    std::string pong;
    proto::append_u32(pong, serial);
    outgoing_.append(proto::message(wm_base_, proto::kXdgWmBasePong, pong));
    return;
  }

  if (header.object == xdg_surface_ && header.opcode == proto::kXdgSurfaceConfigure) {
    std::uint32_t serial = 0;
    if (!reader.u32(serial)) return;
    last_configure_serial_ = serial;
    configured_ = true;
    std::string ack;
    proto::append_u32(ack, serial);
    outgoing_.append(proto::message(xdg_surface_, proto::kXdgSurfaceAckConfigure, ack));
    // The size itself arrives in xdg_toplevel.configure, which immediately follows
    // this event; that is where the Configure event is reported.
    return;
  }

  if (header.object == xdg_toplevel_) {
    if (header.opcode == proto::kXdgToplevelConfigure) {
      proto::Configure configure;
      if (!proto::decode_configure(body, configure)) return;
      if (configure.width > 0 && configure.height > 0) {
        pending_width_ = configure.width;
        pending_height_ = configure.height;
        width_ = configure.width;
        height_ = configure.height;
      }
      queue_.push_back(Event{EventType::Configure, 0, 0, width_, height_, 0, 0});
      return;
    }
    if (header.opcode == proto::kXdgToplevelClose) {
      queue_.push_back(Event{EventType::Close, 0, 0, 0, 0, 0, 0});
      return;
    }
    return;
  }

  if (header.object == seat_ && header.opcode == proto::kSeatCapabilities) {
    std::uint32_t capabilities = 0;
    if (!reader.u32(capabilities)) return;
    if ((capabilities & proto::kSeatCapabilityPointer) != 0 && pointer_ == 0) {
      pointer_ = next_id_++;
      std::string body_bytes;
      proto::append_u32(body_bytes, pointer_);
      outgoing_.append(proto::message(seat_, proto::kSeatGetPointer, body_bytes));
    }
    if ((capabilities & proto::kSeatCapabilityKeyboard) != 0 && keyboard_ == 0) {
      keyboard_ = next_id_++;
      std::string body_bytes;
      proto::append_u32(body_bytes, keyboard_);
      outgoing_.append(proto::message(seat_, proto::kSeatGetKeyboard, body_bytes));
    }
    return;
  }

  if (header.object == pointer_ && pointer_ != 0) {
    switch (header.opcode) {
      case proto::kPointerEnter: {
        std::uint32_t serial = 0;
        std::uint32_t surface = 0;
        int x = 0;
        int y = 0;
        if (!reader.u32(serial) || !reader.u32(surface) || !reader.fixed(x) || !reader.fixed(y)) return;
        last_pointer_x_ = x;
        last_pointer_y_ = y;
        queue_.push_back(Event{EventType::Motion, x, y, 0, 0, 0, 0});
        return;
      }
      case proto::kPointerMotion: {
        std::uint32_t time = 0;
        int x = 0;
        int y = 0;
        if (!reader.u32(time) || !reader.fixed(x) || !reader.fixed(y)) return;
        last_pointer_x_ = x;
        last_pointer_y_ = y;
        queue_.push_back(Event{EventType::Motion, x, y, 0, 0, 0, 0});
        return;
      }
      case proto::kPointerButton: {
        std::uint32_t serial = 0;
        std::uint32_t time = 0;
        std::uint32_t button = 0;
        std::uint32_t state = 0;
        if (!reader.u32(serial) || !reader.u32(time) || !reader.u32(button) || !reader.u32(state)) return;
        if (state != 1) return;  // only presses matter to the window loop
        int mapped = 0;
        if (button == proto::kPointerButtonLeft) {
          mapped = 1;
        } else if (button == proto::kPointerButtonMiddle) {
          mapped = 2;
        } else if (button == proto::kPointerButtonRight) {
          mapped = 3;
        } else {
          return;
        }
        queue_.push_back(Event{EventType::ButtonPress, last_pointer_x_, last_pointer_y_, 0, 0, mapped, 0});
        return;
      }
      case proto::kPointerAxis: {
        std::uint32_t time = 0;
        std::uint32_t axis = 0;
        std::int32_t value = 0;
        if (!reader.u32(time) || !reader.u32(axis) || !reader.i32(value)) return;
        if (axis != 0) return;  // 0 = vertical
        const int wheel = value < 0 ? 4 : 5;  // negative = "up"
        queue_.push_back(Event{EventType::ButtonPress, last_pointer_x_, last_pointer_y_, 0, 0, wheel, 0});
        return;
      }
      default:
        return;
    }
  }

  if (header.object == keyboard_ && keyboard_ != 0 && header.opcode == proto::kKeyboardKey) {
    std::uint32_t serial = 0;
    std::uint32_t time = 0;
    std::uint32_t key = 0;
    std::uint32_t state = 0;
    if (!reader.u32(serial) || !reader.u32(time) || !reader.u32(key) || !reader.u32(state)) return;
    if (state != 1) return;
    const std::uint32_t keysym = proto::keysym_for_evdev(key);
    if (keysym == 0) return;
    queue_.push_back(Event{EventType::KeyPress, 0, 0, 0, 0, 0, keysym});
    return;
  }

  if (header.object == sync_callback_ && header.opcode == proto::kCallbackDone) {
    sync_done_ = true;
    return;
  }

  for (int slot = 0; slot < 2; ++slot) {
    if (buffers_[slot] != 0 && header.object == buffers_[slot] && header.opcode == proto::kBufferRelease) {
      buffer_busy_[slot] = false;
      return;
    }
  }
}

void Window::destroy_buffers() {
  if (fd_ >= 0) {
    for (int slot = 0; slot < 2; ++slot) {
      if (buffers_[slot] != 0) {
        outgoing_.append(proto::message(buffers_[slot], proto::kBufferDestroy));
        buffers_[slot] = 0;
      }
      buffer_busy_[slot] = false;
    }
    if (pool_ != 0) {
      outgoing_.append(proto::message(pool_, proto::kBufferDestroy));
      pool_ = 0;
    }
  }
  if (shm_map_ != nullptr) {
    ::munmap(shm_map_, shm_bytes_);
    shm_map_ = nullptr;
  }
  shm_bytes_ = 0;
  if (shm_fd_ >= 0) {
    ::close(shm_fd_);
    shm_fd_ = -1;
  }
  buffer_width_ = 0;
  buffer_height_ = 0;
  active_slot_ = 0;
}

Status Window::ensure_buffers(int width, int height) {
  if (width <= 0 || height <= 0) return Status::failure(wayland_error("the surface has an empty size"));
  if (pool_ != 0 && buffer_width_ == width && buffer_height_ == height) return Status::success();

  destroy_buffers();
  const std::uint32_t stride = static_cast<std::uint32_t>(width) * 4u;
  const std::size_t slot_bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
  const std::size_t total = slot_bytes * 2;  // double buffering

  shm_fd_ = create_shm_file(total);
  if (shm_fd_ < 0) return Status::failure(wayland_error("cannot create a shared memory file"));
  void* mapped = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (mapped == MAP_FAILED) {
    ::close(shm_fd_);
    shm_fd_ = -1;
    return Status::failure(wayland_error("cannot map the shared memory file"));
  }
  shm_map_ = mapped;
  shm_bytes_ = total;
  std::memset(shm_map_, 0, total);

  pool_ = next_id_++;
  Status status = write_with_fd(proto::create_pool_message(pool_, static_cast<std::uint32_t>(total)), shm_fd_);
  if (status.is_error()) return status;

  for (int slot = 0; slot < 2; ++slot) {
    buffers_[slot] = next_id_++;
    outgoing_.append(proto::create_buffer_message(pool_, buffers_[slot],
                                                  static_cast<std::uint32_t>(slot_bytes), width, height, stride,
                                                  proto::kShmFormatXrgb8888));
    buffer_busy_[slot] = false;
  }
  buffer_width_ = width;
  buffer_height_ = height;
  active_slot_ = 0;
  return flush();
}

Status Window::present(const paint::Canvas& canvas) {
  if (fd_ < 0 || surface_ == 0) return Status::failure(wayland_error("the window is closed"));
  const int width = canvas.width();
  const int height = canvas.height();
  Status status = ensure_buffers(width, height);
  if (status.is_error()) return status;

  // Alternate between the two buffers; if the other one is still in use by the
  // compositor, reuse the one we are already showing (no tear-free guarantee, but the
  // frame is never dropped and we never block the event loop).
  int slot = active_slot_ == 0 ? 1 : 0;
  if (buffer_busy_[slot]) slot = active_slot_;
  buffer_busy_[slot] = true;
  active_slot_ = slot;

  const std::uint32_t stride = static_cast<std::uint32_t>(width) * 4u;
  const std::size_t slot_bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
  auto* target = reinterpret_cast<std::uint8_t*>(shm_map_) + static_cast<std::size_t>(slot) * slot_bytes;
  // Wayland's XRGB8888 is little-endian BGRX, the same layout the X11 backend uploads.
  const std::vector<std::uint8_t>& pixels = canvas.pixels();
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    target[i + 0] = pixels[i + 2];
    target[i + 1] = pixels[i + 1];
    target[i + 2] = pixels[i + 0];
    target[i + 3] = 0xFF;  // X in XRGB8888 is ignored, but keep it opaque anyway
  }

  std::string body;
  proto::append_u32(body, buffers_[slot]);
  proto::append_u32(body, 0);  // attach at (0, 0)
  outgoing_.append(proto::message(surface_, proto::kSurfaceAttach, body));

  body.clear();
  proto::append_u32(body, 0);
  proto::append_u32(body, 0);
  proto::append_u32(body, static_cast<std::uint32_t>(width));
  proto::append_u32(body, static_cast<std::uint32_t>(height));
  outgoing_.append(proto::message(surface_, proto::kSurfaceDamage, body));

  outgoing_.append(proto::message(surface_, proto::kSurfaceCommit));
  return dispatch(0);
}

Event Window::poll(int timeout_ms) {
  if (fd_ < 0) return Event{};
  if (queue_.empty()) {
    const Status status = dispatch(timeout_ms);
    if (status.is_error()) {
      // A dead connection is a closed window as far as the caller is concerned.
      queue_.push_back(Event{EventType::Close, 0, 0, 0, 0, 0, 0});
      display_error_.clear();
    }
  }
  if (queue_.empty()) return Event{};
  const Event event = queue_.front();
  queue_.erase(queue_.begin());
  return event;
}

void Window::close() {
  if (fd_ >= 0) {
    destroy_buffers();
    if (surface_ != 0) outgoing_.append(proto::message(surface_, 0));  // wl_surface.destroy
    if (xdg_toplevel_ != 0) outgoing_.append(proto::message(xdg_toplevel_, 0));
    if (xdg_surface_ != 0) outgoing_.append(proto::message(xdg_surface_, 0));
    if (decoration_ != 0) outgoing_.append(proto::message(decoration_, 0));  // destroy
    (void)flush();
  }
  if (shm_map_ != nullptr) {
    ::munmap(shm_map_, shm_bytes_);
    shm_map_ = nullptr;
  }
  shm_bytes_ = 0;
  if (shm_fd_ >= 0) {
    ::close(shm_fd_);
    shm_fd_ = -1;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  surface_ = 0;
  xdg_surface_ = 0;
  xdg_toplevel_ = 0;
  pool_ = 0;
  buffers_[0] = 0;
  buffers_[1] = 0;
  queue_.clear();
}

}  // namespace bonk::shell::wayland
