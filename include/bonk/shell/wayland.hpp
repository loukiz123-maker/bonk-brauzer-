// Bonk Brauzer — a window backend written directly against the Wayland wire protocol.
//
// No libwayland, no libxkbcommon, no SDL: we open the compositor's unix socket and
// speak the protocol ourselves, exactly like the X11 backend in `x11.hpp`. The subset
// implemented here is:
//   wl_display (get_registry, sync, error), wl_registry (bind, global),
//   wl_compositor (create_surface), wl_shm / wl_shm_pool / wl_buffer
//   (create_pool over SCM_RIGHTS, create_buffer, release),
//   xdg_wm_base (get_xdg_surface, pong, ping), xdg_surface (get_toplevel,
//   ack_configure, configure), xdg_toplevel (set_title, set_app_id, configure, close),
//   wl_seat (get_pointer, get_keyboard, capabilities), wl_pointer (enter, motion,
//   button, axis) and wl_keyboard (key).
//
// The encoding/decoding functions are public so they can be unit tested without a
// compositor (tests/shell/wayland_test.cpp) — the same rule the X11 backend follows.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"
#include "bonk/paint/canvas.hpp"

namespace bonk::shell::wayland {

enum class EventType { None, Expose, Configure, ButtonPress, Motion, KeyPress, Close };

struct Event {
  EventType type = EventType::None;
  int x = 0;                 // pointer position inside the surface (ButtonPress/Motion)
  int y = 0;
  int width = 0;             // new surface size (Configure)
  int height = 0;
  int button = 0;            // 1 = left, 2 = middle, 3 = right, 4/5 = wheel up/down
  std::uint32_t keysym = 0;  // KeyPress
};

namespace proto {

// Wayland's wire format: `object id` (u32), then `size << 16 | opcode` (u32); the size
// counts the header itself and must be a multiple of four.
inline constexpr std::uint32_t kDisplayId = 1;
inline constexpr std::uint32_t kVersion = 1;  // every global we bind is asked for v1
inline constexpr std::uint32_t kShmFormatXrgb8888 = 1;
inline constexpr std::uint32_t kShmFormatArgb8888 = 0;
inline constexpr std::uint32_t kSeatCapabilityPointer = 1;
inline constexpr std::uint32_t kSeatCapabilityKeyboard = 2;
inline constexpr std::uint32_t kPointerButtonLeft = 0x110;   // BTN_LEFT
inline constexpr std::uint32_t kPointerButtonRight = 0x111;  // BTN_RIGHT
inline constexpr std::uint32_t kPointerButtonMiddle = 0x112; // BTN_MIDDLE

// Requests.
inline constexpr std::uint16_t kDisplayError = 0;
inline constexpr std::uint16_t kDisplayGetRegistry = 1;
inline constexpr std::uint16_t kDisplaySync = 0;
inline constexpr std::uint16_t kRegistryBind = 0;
inline constexpr std::uint16_t kRegistryGlobal = 0;
inline constexpr std::uint16_t kRegistryGlobalRemove = 1;
inline constexpr std::uint16_t kCallbackDone = 0;
inline constexpr std::uint16_t kCompositorCreateSurface = 0;
inline constexpr std::uint16_t kShmCreatePool = 0;
inline constexpr std::uint16_t kShmPoolCreateBuffer = 0;
inline constexpr std::uint16_t kShmPoolDestroy = 1;
inline constexpr std::uint16_t kShmFormat = 0;
inline constexpr std::uint16_t kBufferDestroy = 0;
inline constexpr std::uint16_t kBufferRelease = 0;
inline constexpr std::uint16_t kSurfaceAttach = 1;
inline constexpr std::uint16_t kSurfaceDamage = 2;
inline constexpr std::uint16_t kSurfaceCommit = 6;
inline constexpr std::uint16_t kXdgWmBaseGetXdgSurface = 2;
inline constexpr std::uint16_t kXdgWmBasePong = 3;
inline constexpr std::uint16_t kXdgWmBasePing = 0;
inline constexpr std::uint16_t kXdgSurfaceGetToplevel = 1;
inline constexpr std::uint16_t kXdgSurfaceAckConfigure = 4;
inline constexpr std::uint16_t kXdgSurfaceConfigure = 0;
inline constexpr std::uint16_t kXdgToplevelSetTitle = 2;
inline constexpr std::uint16_t kXdgToplevelSetAppId = 3;
inline constexpr std::uint16_t kXdgToplevelConfigure = 0;
inline constexpr std::uint16_t kXdgToplevelClose = 1;
// zxdg_decoration_manager_v1 / zxdg_toplevel_decoration_v1 (unstable protocol KWin and
// Mutter both offer): without it a toplevel gets no title bar at all.
inline constexpr std::uint16_t kXdgDecorationGetToplevelDecoration = 1;
inline constexpr std::uint16_t kXdgDecorationSetMode = 1;
inline constexpr std::uint32_t kXdgDecorationModeServerSide = 1;
inline constexpr std::uint16_t kSeatGetPointer = 0;
inline constexpr std::uint16_t kSeatGetKeyboard = 1;
inline constexpr std::uint16_t kSeatCapabilities = 0;
inline constexpr std::uint16_t kSeatName = 1;
inline constexpr std::uint16_t kPointerSetCursor = 0;
inline constexpr std::uint16_t kPointerEnter = 0;
inline constexpr std::uint16_t kPointerLeave = 1;
inline constexpr std::uint16_t kPointerMotion = 2;
inline constexpr std::uint16_t kPointerButton = 3;
inline constexpr std::uint16_t kPointerAxis = 4;
inline constexpr std::uint16_t kKeyboardKeymap = 0;
inline constexpr std::uint16_t kKeyboardKey = 3;

/// Arguments are little-endian and padded so that the next one stays aligned.
void append_u32(std::string& out, std::uint32_t value);
std::string string_arg(std::string_view text);            // u32 length incl. NUL + bytes + pad
std::string array_arg(std::string_view bytes);            // u32 byte count + bytes + pad
std::string message(std::uint32_t object, std::uint16_t opcode, std::string_view body = {});
std::string bind_message(std::uint32_t registry, std::uint32_t name, std::string_view interface, std::uint32_t version,
                         std::uint32_t id);
std::string sync_message(std::uint32_t callback);
std::string create_pool_message(std::uint32_t pool, std::uint32_t size);
std::string create_buffer_message(std::uint32_t pool, std::uint32_t buffer, std::uint32_t offset, int width,
                                  int height, std::uint32_t stride, std::uint32_t format);

/// A decoded message header. `size` includes the 8-byte header.
struct Header {
  std::uint32_t object = 0;
  std::uint16_t size = 0;
  std::uint16_t opcode = 0;
  [[nodiscard]] bool is_valid() const noexcept { return size >= 8 && size % 4 == 0; }
};

/// Reads one header at `offset`; false when fewer than 8 bytes are available.
bool decode_header(std::string_view bytes, std::size_t offset, Header& header);

/// Cursor into a message body: every reader advances and refuses to run past the end,
/// so a malformed message yields `false` instead of reading garbage.
class Reader {
 public:
  explicit Reader(std::string_view body) : body_(body) {}
  bool u32(std::uint32_t& value);
  bool i32(std::int32_t& value);
  bool string(std::string& value);
  /// Fixed-point 24.8 → integer part, rounded towards zero.
  bool fixed(int& value);
  [[nodiscard]] bool at_end() const noexcept { return position_ >= body_.size(); }

 private:
  std::string_view body_;
  std::size_t position_ = 0;
};

/// xdg_toplevel.configure: `width`, `height`, then the states array.
struct Configure {
  int width = 0;
  int height = 0;
};
bool decode_configure(std::string_view body, Configure& configure);

/// evdev keycode → X11 keysym for a US layout. The window loop only needs arrows and
/// navigation keys, but letters/digits are mapped too so scripted key handling works.
std::uint32_t keysym_for_evdev(std::uint32_t code);

}  // namespace proto

/// An open Wayland surface (with its `xdg_toplevel`).
class Window {
 public:
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  /// Connects to `$WAYLAND_DISPLAY` (or `display_name`, which may be an absolute path),
  /// binds the globals we need and creates an `xdg_toplevel`.
  static Result<std::unique_ptr<Window>> open(int width, int height, std::string_view title,
                                              std::string_view display_name = {});

  /// Copies a canvas into the shared buffer and commits it (wl_surface.attach/commit).
  Status present(const paint::Canvas& canvas);

  /// Handles incoming protocol messages, blocking up to `timeout_ms` when the queue is
  /// empty; EventType::None means "nothing happened yet".
  Event poll(int timeout_ms);

  void close();

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t surface() const noexcept { return surface_; }
  [[nodiscard]] bool configured() const noexcept { return configured_; }

 private:
  Window() = default;

  Status connect_display(std::string_view display_name);
  Status bind_globals();
  Status create_surface(std::string_view title);
  Status roundtrip();
  Status flush();
  Status write_with_fd(std::string_view bytes, int fd);
  Status dispatch(int timeout_ms);
  void handle_message(const proto::Header& header, std::string_view body);
  Status ensure_buffers(int width, int height);
  void destroy_buffers();

  int fd_ = -1;
  int shm_fd_ = -1;
  void* shm_map_ = nullptr;
  std::size_t shm_bytes_ = 0;

  std::uint32_t next_id_ = 2;
  std::uint32_t registry_ = 0;
  std::uint32_t compositor_ = 0;
  std::uint32_t shm_ = 0;
  std::uint32_t wm_base_ = 0;
  std::uint32_t seat_ = 0;
  std::uint32_t pointer_ = 0;
  std::uint32_t keyboard_ = 0;
  std::uint32_t surface_ = 0;
  std::uint32_t xdg_surface_ = 0;
  std::uint32_t xdg_toplevel_ = 0;
  std::uint32_t decoration_manager_ = 0;
  std::uint32_t decoration_ = 0;
  std::uint32_t pool_ = 0;
  std::uint32_t buffers_[2] = {0, 0};
  bool buffer_busy_[2] = {false, false};
  int active_slot_ = 0;
  int last_pointer_x_ = 0;
  int last_pointer_y_ = 0;
  int buffer_width_ = 0;
  int buffer_height_ = 0;

  std::uint32_t sync_callback_ = 0;
  bool sync_done_ = false;
  bool configured_ = false;
  std::uint32_t last_configure_serial_ = 0;
  std::int32_t pending_width_ = 0;
  std::int32_t pending_height_ = 0;
  std::string display_error_;

  std::string outgoing_;
  std::string incoming_;
  std::vector<Event> queue_;
  int width_ = 0;
  int height_ = 0;
};

/// True when `$WAYLAND_DISPLAY` (or `$XDG_RUNTIME_DIR/wayland-0`) looks usable.
[[nodiscard]] bool display_available();

/// Absolute socket path for a display name, or an empty string when it cannot be resolved.
[[nodiscard]] std::string socket_path(std::string_view display_name = {});

}  // namespace bonk::shell::wayland
