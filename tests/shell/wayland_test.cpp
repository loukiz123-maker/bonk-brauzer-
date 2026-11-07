// Stage 34: the Wayland backend.
//
// Two layers are tested here:
//   1. the wire-format helpers (`proto::*`) — pure functions, no compositor needed;
//   2. the whole window handshake against a mock compositor that speaks just enough of
//      the protocol (registry, bind, sync, xdg_shell, wl_shm, wl_seat, input events).
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include "bonk/paint/canvas.hpp"
#include "bonk/shell/wayland.hpp"

using namespace bonk;
using namespace bonk::shell::wayland;

namespace {

constexpr std::uint32_t kConfigureSerial = 7;
constexpr int kToplevelWidth = 400;
constexpr int kToplevelHeight = 300;

std::uint32_t u32_at(const std::string& data, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3])) << 24);
}

/// A compositor stand-in: it answers the handshake and then pushes input events, so the
/// backend can be exercised end to end without a display server.
class MockCompositor {
 public:
  MockCompositor() {
    std::string path = "/tmp/bonk-wayland-test-XXXXXX";
    std::vector<char> writable(path.begin(), path.end());
    writable.push_back('\0');
    const int placeholder = ::mkstemp(writable.data());
    if (placeholder >= 0) ::close(placeholder);
    ::unlink(writable.data());
    path_ = writable.data();

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1);
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::listen(listen_fd_, 1);
  }

  ~MockCompositor() {
    stop();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    ::unlink(path_.c_str());
  }

  MockCompositor(const MockCompositor&) = delete;
  MockCompositor& operator=(const MockCompositor&) = delete;

  void start() { thread_ = std::thread([this] { run(); }); }
  void stop() {
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] int commits() const { return commits_; }
  [[nodiscard]] bool acked() const { return acked_; }
  [[nodiscard]] std::uint32_t decoration_mode() const { return decoration_mode_; }

 private:
  void send(const std::string& bytes) {
    if (fd_ < 0 || bytes.empty()) return;
    (void)::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  }

  void send_global(std::uint32_t registry, std::uint32_t name, const char* interface, std::uint32_t version) {
    std::string body;
    proto::append_u32(body, name);
    body.append(proto::string_arg(interface));
    proto::append_u32(body, version);
    send(proto::message(registry, proto::kRegistryGlobal, body));
  }

  void send_input() {
    // wl_pointer.enter(serial, surface, surface_x, surface_y) with 24.8 fixed point.
    std::string body;
    proto::append_u32(body, 11);
    proto::append_u32(body, surface_);
    proto::append_u32(body, 21 * 256 + 128);
    proto::append_u32(body, 13 * 256 + 64);
    send(proto::message(pointer_, proto::kPointerEnter, body));

    body.clear();
    proto::append_u32(body, 5);
    proto::append_u32(body, 21 * 256 + 128);
    proto::append_u32(body, 13 * 256 + 64);
    send(proto::message(pointer_, proto::kPointerMotion, body));

    body.clear();
    proto::append_u32(body, 12);
    proto::append_u32(body, 6);
    proto::append_u32(body, proto::kPointerButtonLeft);
    proto::append_u32(body, 1);  // pressed
    send(proto::message(pointer_, proto::kPointerButton, body));

    body.clear();
    proto::append_u32(body, 13);
    proto::append_u32(body, 7);
    proto::append_u32(body, 105);  // KEY_LEFT
    proto::append_u32(body, 1);    // pressed
    send(proto::message(keyboard_, proto::kKeyboardKey, body));

    send(proto::message(toplevel_, proto::kXdgToplevelClose));
  }

  void handle(const proto::Header& header, std::string_view body) {
    proto::Reader reader(body);
    if (header.object == proto::kDisplayId && header.opcode == proto::kDisplayGetRegistry) {
      std::uint32_t registry = 0;
      if (!reader.u32(registry)) return;
      registry_ = registry;
      send_global(registry_, 1, "wl_compositor", 4);
      send_global(registry_, 2, "wl_shm", 1);
      send_global(registry_, 3, "xdg_wm_base", 1);
      send_global(registry_, 4, "wl_seat", 5);
      send_global(registry_, 5, "zxdg_decoration_manager_v1", 1);
      return;
    }
    if (header.object == proto::kDisplayId && header.opcode == proto::kDisplaySync) {
      std::uint32_t callback = 0;
      if (!reader.u32(callback)) return;
      send(proto::message(callback, proto::kCallbackDone));
      return;
    }
    if (header.object == registry_ && header.opcode == proto::kRegistryBind) {
      std::uint32_t name = 0;
      std::string interface;
      std::uint32_t version = 0;
      std::uint32_t id = 0;
      if (!reader.u32(name) || !reader.string(interface) || !reader.u32(version) || !reader.u32(id)) return;
      bound_versions_.push_back(version);
      if (interface == "wl_compositor") {
        compositor_ = id;
      } else if (interface == "wl_shm") {
        shm_ = id;
      } else if (interface == "xdg_wm_base") {
        wm_base_ = id;
      } else if (interface == "zxdg_decoration_manager_v1") {
        decoration_manager_ = id;
      } else if (interface == "wl_seat") {
        seat_ = id;
        std::string capabilities;
        proto::append_u32(capabilities, proto::kSeatCapabilityPointer | proto::kSeatCapabilityKeyboard);
        send(proto::message(seat_, proto::kSeatCapabilities, capabilities));
      }
      return;
    }
    if (header.object == compositor_ && header.opcode == proto::kCompositorCreateSurface) {
      (void)reader.u32(surface_);
      return;
    }
    if (header.object == wm_base_ && header.opcode == proto::kXdgWmBaseGetXdgSurface) {
      (void)reader.u32(xdg_surface_);
      std::uint32_t surface = 0;
      (void)reader.u32(surface);
      surface_ = surface;
      return;
    }
    if (header.object == xdg_surface_ && header.opcode == proto::kXdgSurfaceGetToplevel) {
      (void)reader.u32(toplevel_);
      std::string configure;
      proto::append_u32(configure, kConfigureSerial);
      send(proto::message(xdg_surface_, proto::kXdgSurfaceConfigure, configure));
      std::string toplevel;
      proto::append_u32(toplevel, static_cast<std::uint32_t>(kToplevelWidth));
      proto::append_u32(toplevel, static_cast<std::uint32_t>(kToplevelHeight));
      proto::append_u32(toplevel, 0);  // empty states array
      send(proto::message(toplevel_, proto::kXdgToplevelConfigure, toplevel));
      return;
    }
    if (header.object == xdg_surface_ && header.opcode == proto::kXdgSurfaceAckConfigure) {
      std::uint32_t serial = 0;
      if (reader.u32(serial) && serial == kConfigureSerial) acked_ = true;
      return;
    }
    if (header.object == surface_ && header.opcode == proto::kSurfaceCommit) {
      ++commits_;
      return;
    }
    if (header.object == decoration_manager_ && header.opcode == proto::kXdgDecorationGetToplevelDecoration) {
      (void)reader.u32(decoration_);
      std::uint32_t toplevel = 0;
      (void)reader.u32(toplevel);
      return;
    }
    if (header.object == decoration_ && header.opcode == proto::kXdgDecorationSetMode) {
      std::uint32_t mode = 0;
      if (reader.u32(mode)) decoration_mode_ = mode;
      return;
    }
    if (header.object == seat_ && header.opcode == proto::kSeatGetPointer) {
      (void)reader.u32(pointer_);
      return;
    }
    if (header.object == seat_ && header.opcode == proto::kSeatGetKeyboard) {
      (void)reader.u32(keyboard_);
      return;
    }
  }

  void run() {
    fd_ = ::accept(listen_fd_, nullptr, nullptr);
    if (fd_ < 0) return;
    std::string buffer;
    bool sent = false;
    for (;;) {
      char chunk[4096];
      const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (received <= 0) break;
      buffer.append(chunk, static_cast<std::size_t>(received));
      std::size_t offset = 0;
      while (true) {
        proto::Header header;
        if (!proto::decode_header(buffer, offset, header)) break;
        if (offset + header.size > buffer.size()) break;
        handle(header, std::string_view(buffer).substr(offset + 8, static_cast<std::size_t>(header.size) - 8));
        offset += header.size;
      }
      if (offset > 0) buffer.erase(0, offset);
      if (!sent && acked_ && pointer_ != 0 && keyboard_ != 0 && commits_ > 1) {
        sent = true;
        send_input();
      }
    }
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  std::string path_;
  int listen_fd_ = -1;
  int fd_ = -1;
  std::thread thread_;
  std::uint32_t registry_ = 0;
  std::uint32_t compositor_ = 0;
  std::uint32_t shm_ = 0;
  std::uint32_t wm_base_ = 0;
  std::uint32_t seat_ = 0;
  std::uint32_t pointer_ = 0;
  std::uint32_t keyboard_ = 0;
  std::uint32_t surface_ = 0;
  std::uint32_t xdg_surface_ = 0;
  std::uint32_t toplevel_ = 0;
  std::uint32_t decoration_manager_ = 0;
  std::uint32_t decoration_ = 0;
  std::uint32_t decoration_mode_ = 0;
  int commits_ = 0;
  bool acked_ = false;
  std::vector<std::uint32_t> bound_versions_;
};

}  // namespace

// ---------------------------------------------------------------- wire format ----

BONK_TEST(wayland_proto, message_header_and_size) {
  const std::string bytes = proto::message(proto::kDisplayId, proto::kSurfaceCommit);
  CHECK_EQ(bytes.size(), std::size_t{8});
  CHECK_EQ(u32_at(bytes, 0), proto::kDisplayId);
  const std::uint32_t second = u32_at(bytes, 4);
  CHECK_EQ(second >> 16, 8u);                       // size includes the header
  CHECK_EQ(second & 0xFFFF, proto::kSurfaceCommit);  // opcode in the low half

  proto::Header header;
  CHECK(proto::decode_header(bytes, 0, header));
  CHECK_EQ(header.object, proto::kDisplayId);
  CHECK_EQ(header.opcode, proto::kSurfaceCommit);
  CHECK_EQ(header.size, std::uint16_t{8});
  CHECK(header.is_valid());
  CHECK(!proto::decode_header(bytes, 1, header));  // a partial header is not an error
}

BONK_TEST(wayland_proto, string_arguments_are_nul_terminated_and_padded) {
  const std::string body = proto::string_arg("ab");
  CHECK_EQ(body.size(), std::size_t{8});       // length + "ab\0" + one pad byte
  CHECK_EQ(u32_at(body, 0), 3u);               // the NUL is part of the length
  CHECK_EQ(body.substr(4, 4), std::string("ab\0\0", 4));

  const std::string aligned = proto::string_arg("abcd");
  CHECK_EQ(aligned.size(), std::size_t{12});

  proto::Reader reader(body);
  std::string text;
  CHECK(reader.string(text));
  CHECK_EQ(text, std::string("ab"));
}

BONK_TEST(wayland_proto, reader_refuses_to_run_past_the_end) {
  std::string body;
  proto::append_u32(body, 42);
  proto::Reader reader(body);
  std::uint32_t value = 0;
  CHECK(reader.u32(value));
  CHECK_EQ(value, 42u);
  CHECK(!reader.u32(value));  // only four bytes were available
  CHECK(reader.at_end());
}

BONK_TEST(wayland_proto, bind_and_buffer_messages) {
  const std::string bind = proto::bind_message(3, 12, "xdg_wm_base", 1, 9);
  proto::Header header;
  CHECK(proto::decode_header(bind, 0, header));
  CHECK_EQ(header.object, 3u);
  CHECK_EQ(header.opcode, proto::kRegistryBind);
  CHECK_EQ(bind.size(), std::size_t{header.size});
  proto::Reader reader(std::string_view(bind).substr(8));
  std::uint32_t name = 0;
  std::string interface;
  std::uint32_t version = 0;
  std::uint32_t id = 0;
  CHECK(reader.u32(name));
  CHECK(reader.string(interface));
  CHECK(reader.u32(version));
  CHECK(reader.u32(id));
  CHECK_EQ(name, 12u);
  CHECK_EQ(interface, std::string("xdg_wm_base"));
  CHECK_EQ(version, 1u);
  CHECK_EQ(id, 9u);

  const std::string buffer = proto::create_buffer_message(5, 6, 0, 640, 480, 2560, proto::kShmFormatXrgb8888);
  proto::Header buffer_header;
  CHECK(proto::decode_header(buffer, 0, buffer_header));
  CHECK_EQ(buffer_header.object, 5u);
  CHECK_EQ(buffer_header.opcode, proto::kShmPoolCreateBuffer);
  proto::Reader fields(std::string_view(buffer).substr(8));
  std::uint32_t buffer_id = 0;
  std::uint32_t offset = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::uint32_t stride = 0;
  std::uint32_t format = 0;
  CHECK(fields.u32(buffer_id));
  CHECK(fields.u32(offset));
  CHECK(fields.i32(width));
  CHECK(fields.i32(height));
  CHECK(fields.u32(stride));
  CHECK(fields.u32(format));
  CHECK_EQ(buffer_id, 6u);
  CHECK_EQ(width, 640);
  CHECK_EQ(stride, 2560u);
  CHECK_EQ(format, proto::kShmFormatXrgb8888);
}

BONK_TEST(wayland_proto, configure_and_keysyms) {
  std::string body;
  proto::append_u32(body, 800);
  proto::append_u32(body, 600);
  proto::append_u32(body, 0);  // states array
  proto::Configure configure;
  CHECK(proto::decode_configure(body, configure));
  CHECK_EQ(configure.width, 800);
  CHECK_EQ(configure.height, 600);

  // A zero configure means "keep the current size".
  std::string zero;
  proto::append_u32(zero, 0);
  proto::append_u32(zero, 0);
  CHECK(proto::decode_configure(zero, configure));
  CHECK_EQ(configure.width, 0);
  CHECK(!proto::decode_configure(std::string_view(zero).substr(0, 6), configure));

  CHECK_EQ(proto::keysym_for_evdev(108), 0xFF54u);  // Down
  CHECK_EQ(proto::keysym_for_evdev(103), 0xFF52u);  // Up
  CHECK_EQ(proto::keysym_for_evdev(104), 0xFF55u);  // Page_Up
  CHECK_EQ(proto::keysym_for_evdev(109), 0xFF56u);  // Page_Down
  CHECK_EQ(proto::keysym_for_evdev(50), static_cast<std::uint32_t>('m'));
  CHECK_EQ(proto::keysym_for_evdev(11), static_cast<std::uint32_t>('0'));
  CHECK_EQ(proto::keysym_for_evdev(57), static_cast<std::uint32_t>(' '));
  CHECK_EQ(proto::keysym_for_evdev(200), 0u);  // unmapped
}

BONK_TEST(wayland_proto, socket_paths) {
  // An absolute name is taken verbatim (that is how the tests point at a mock server).
  CHECK_EQ(socket_path("/tmp/some-compositor"), std::string("/tmp/some-compositor"));
  const char* saved_runtime = std::getenv("XDG_RUNTIME_DIR");
  const std::string saved = saved_runtime != nullptr ? std::string(saved_runtime) : std::string();
  ::setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
  CHECK_EQ(socket_path("wayland-1"), std::string("/run/user/1000/wayland-1"));
  if (saved.empty()) {
    ::unsetenv("XDG_RUNTIME_DIR");
  } else {
    ::setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
  }
}

// --------------------------------------------------------- end-to-end window ----

BONK_TEST(wayland_window, handshake_present_and_input) {
  MockCompositor compositor;
  compositor.start();

  Result<std::unique_ptr<Window>> window = Window::open(320, 240, "bonk test", compositor.path());
  CHECK(window.is_ok());
  if (window.is_error()) {
    compositor.stop();
    return;
  }
  CHECK(window.value()->configured());
  // The compositor's xdg_toplevel.configure wins over the requested size.
  CHECK_EQ(window.value()->width(), kToplevelWidth);
  CHECK_EQ(window.value()->height(), kToplevelHeight);
  CHECK(window.value()->surface() != 0);

  paint::Canvas canvas(64, 48);
  canvas.clear(css::Color{10, 20, 30, 255});
  CHECK(window.value()->present(canvas).is_ok());

  // The first event is the configure that arrived before the loop started.
  const Event configure = window.value()->poll(2000);
  CHECK_EQ(static_cast<int>(configure.type), static_cast<int>(EventType::Configure));
  CHECK_EQ(configure.width, kToplevelWidth);
  CHECK_EQ(configure.height, kToplevelHeight);

  // Then the mock's pointer/keyboard input: enter → motion → click → Left arrow → close.
  const Event motion = window.value()->poll(2000);
  CHECK_EQ(static_cast<int>(motion.type), static_cast<int>(EventType::Motion));
  CHECK_EQ(motion.x, 21);
  CHECK_EQ(motion.y, 13);

  const Event moved = window.value()->poll(2000);
  CHECK_EQ(static_cast<int>(moved.type), static_cast<int>(EventType::Motion));
  CHECK_EQ(moved.x, 21);
  CHECK_EQ(moved.y, 13);

  const Event button = window.value()->poll(2000);
  CHECK_EQ(static_cast<int>(button.type), static_cast<int>(EventType::ButtonPress));
  CHECK_EQ(button.button, 1);
  CHECK_EQ(button.x, 21);
  CHECK_EQ(button.y, 13);

  const Event key = window.value()->poll(2000);
  CHECK_EQ(static_cast<int>(key.type), static_cast<int>(EventType::KeyPress));
  CHECK_EQ(key.keysym, 0xFF51u);  // Left arrow

  const Event close_event = window.value()->poll(2000);
  CHECK_EQ(static_cast<int>(close_event.type), static_cast<int>(EventType::Close));

  CHECK(compositor.acked());
  CHECK_EQ(compositor.decoration_mode(), proto::kXdgDecorationModeServerSide);
  CHECK(compositor.commits() >= 2);  // the initial commit plus the presented frame
  // Close the connection before joining: the mock server reads until EOF.
  window.value().reset();
  compositor.stop();
}

BONK_TEST(wayland_window, missing_socket_is_reported) {
  Result<std::unique_ptr<Window>> window = Window::open(320, 240, "bonk test", "/tmp/bonk-no-such-compositor.sock");
  CHECK(window.is_error());
  if (window.is_error()) {
    CHECK(window.error().message.find("cannot connect") != std::string::npos);
  }
}
