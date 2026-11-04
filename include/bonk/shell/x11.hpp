// Bonk Brauzer — a window backend written directly against the X11 wire protocol.
//
// No Xlib, no XCB, no SDL: we speak the protocol over the abstract/unix socket,
// exactly like the constraint "only stdlib + an optional window library" allows us
// to avoid entirely. The subset implemented here is:
//   connection setup, InternAtom, CreateWindow, MapWindow, ChangeProperty (title),
//   CreateGC, PutImage (ZPixmap 32-bit), SelectInput, GetKeyboardMapping,
//   and the events Expose / ConfigureNotify / ButtonPress / MotionNotify /
//   KeyPress / ClientMessage(WM_DELETE_WINDOW).
//
// The protocol encoding functions are public so they can be unit tested without a
// display (tests/shell/x11_test.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"
#include "bonk/paint/canvas.hpp"

namespace bonk::shell::x11 {

enum class EventType { None, Expose, Configure, ButtonPress, Motion, KeyPress, Close };

struct Event {
  EventType type = EventType::None;
  int x = 0;          // pointer position inside the window (ButtonPress/Motion)
  int y = 0;
  int width = 0;      // new size (Configure)
  int height = 0;
  int button = 0;     // 1 = left, 4/5 = wheel up/down
  std::uint32_t keysym = 0;  // KeyPress
};

namespace proto {

// Values from the X11 protocol specification (X Protocol Reference Manual).
inline constexpr std::uint8_t kOpCreateWindow = 1;
inline constexpr std::uint8_t kOpMapWindow = 8;
inline constexpr std::uint8_t kOpInternAtom = 16;
inline constexpr std::uint8_t kOpChangeProperty = 18;
inline constexpr std::uint8_t kOpSetInputFocus = 42;
inline constexpr std::uint8_t kOpSelectInput = 43;
inline constexpr std::uint8_t kOpCreateGC = 55;
inline constexpr std::uint8_t kOpPutImage = 72;
inline constexpr std::uint8_t kOpGetKeyboardMapping = 101;

inline constexpr std::uint32_t kEventKeyPress = 1u << 0;
inline constexpr std::uint32_t kEventButtonPress = 1u << 2;
inline constexpr std::uint32_t kEventButtonRelease = 1u << 3;
inline constexpr std::uint32_t kEventPointerMotion = 1u << 6;
inline constexpr std::uint32_t kEventExposure = 1u << 15;
inline constexpr std::uint32_t kEventStructureNotify = 1u << 17;

inline constexpr std::uint32_t kAtomWmName = 39;       // predefined
inline constexpr std::uint32_t kAtomString = 31;       // predefined
inline constexpr std::uint32_t kAtomWmProtocols = 68;  // predefined
inline constexpr std::uint32_t kAtomAtom = 4;          // predefined

/// Appends a little-endian 16/32-bit value (X11 clients on x86 use LSB order).
void append_u16(std::string& out, std::uint16_t value);
void append_u32(std::string& out, std::uint32_t value);
void pad_to_4(std::string& out);

/// Prefixes a request with its length in 4-byte units (the length field is a u16).
void finish_request(std::string& request);

/// Setup request sent right after the socket is opened.
std::string setup_request(std::string_view auth_name, std::string_view auth_data);

std::string intern_atom_request(std::uint16_t sequence, std::string_view name);
std::string create_window_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t parent, int x, int y,
                                  int width, int height, std::uint32_t background_pixel);
std::string map_window_request(std::uint16_t sequence, std::uint32_t window);
std::string select_input_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t event_mask);
std::string create_gc_request(std::uint16_t sequence, std::uint32_t gc, std::uint32_t drawable,
                              std::uint32_t foreground);
std::string change_property_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t property,
                                    std::uint32_t type, std::string_view data);
/// ChangeProperty with format 32 (used for WM_PROTOCOLS): `data` holds 4-byte values.
std::string change_property32_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t property,
                                      std::string_view data);
std::string set_input_focus_request(std::uint16_t sequence, std::uint32_t window);
std::string get_keyboard_mapping_request(std::uint16_t sequence, std::uint8_t first_keycode, std::uint8_t count);
std::string put_image_request(std::uint16_t sequence, std::uint32_t drawable, std::uint32_t gc, int width, int height,
                              std::uint8_t depth, std::string_view pixels);

/// Decodes one 32-byte event packet. Returns EventType::None for events we ignore.
Event decode_event(const std::uint8_t* packet);

}  // namespace proto

/// An open X11 window with a 32-bit ZPixmap drawing target.
class Window {
 public:
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  /// Opens $DISPLAY (or `display_name`), creates and maps a window.
  static Result<std::unique_ptr<Window>> open(int width, int height, std::string_view title,
                                              std::string_view display_name = {});

  /// Uploads a canvas as the whole window contents (X11 PutImage).
  Status present(const paint::Canvas& canvas);

  /// Blocks up to `timeout_ms` for one event; EventType::None on timeout.
  Event poll(int timeout_ms);

  void close();

  [[nodiscard]] std::uint32_t id() const noexcept { return window_; }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t keysym_for(std::uint8_t keycode) const;

 private:
  Window() = default;

  Status handshake(std::string_view display_name);
  Status write_all(std::string_view bytes);
  bool read_exact(void* buffer, std::size_t length, int timeout_ms);
  Status intern_atom(std::string_view name, std::uint32_t& atom);
  Status keyboard_mapping();

  int fd_ = -1;
  std::uint32_t root_ = 0;
  std::uint32_t window_ = 0;
  std::uint32_t gc_ = 0;
  std::uint8_t depth_ = 24;
  std::uint32_t black_pixel_ = 0;
  std::uint16_t sequence_ = 1;
  int width_ = 0;
  int height_ = 0;
  std::uint32_t wm_delete_window_ = 0;
  std::uint8_t min_keycode_ = 8;
  std::vector<std::uint32_t> keysyms_;  // keycode → keysyms_per_keycode entries
  std::uint8_t keysyms_per_keycode_ = 0;
};

/// True when $DISPLAY looks usable (the socket path exists).
[[nodiscard]] bool display_available();

}  // namespace bonk::shell::x11
