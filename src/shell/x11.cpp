#include "bonk/shell/x11.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace bonk::shell::x11 {
namespace proto {

void append_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void append_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void pad_to_4(std::string& out) {
  while (out.size() % 4 != 0) out.push_back('\0');
}

void finish_request(std::string& request) {
  pad_to_4(request);
  const auto units = static_cast<std::uint16_t>((request.size() - 4) / 4);
  request[2] = static_cast<char>(units & 0xFF);
  request[3] = static_cast<char>((units >> 8) & 0xFF);
}

namespace {
std::string request_head(std::uint8_t opcode, std::uint8_t data, std::uint16_t sequence) {
  std::string out;
  out.push_back(static_cast<char>(opcode));
  out.push_back(static_cast<char>(data));
  append_u16(out, sequence);
  return out;
}
}  // namespace

std::string setup_request(std::string_view auth_name, std::string_view auth_data) {
  std::string out;
  out.push_back('l');  // little-endian client
  out.push_back('\0');
  append_u16(out, 11);  // protocol major
  append_u16(out, 0);   // protocol minor
  append_u16(out, static_cast<std::uint16_t>(auth_name.size()));
  append_u16(out, static_cast<std::uint16_t>(auth_data.size()));
  out.push_back('\0');
  out.push_back('\0');
  out.append(auth_name);
  pad_to_4(out);
  out.append(auth_data);
  pad_to_4(out);
  return out;
}

std::string intern_atom_request(std::uint16_t sequence, std::string_view name) {
  std::string out = request_head(kOpInternAtom, 0, sequence);
  append_u16(out, static_cast<std::uint16_t>(name.size()));
  append_u16(out, 0);
  out.append(name);
  finish_request(out);
  return out;
}

std::string create_window_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t parent, int x, int y,
                                  int width, int height, std::uint32_t background_pixel) {
  std::string out = request_head(kOpCreateWindow, 24, sequence);  // depth = CopyFromParent
  append_u32(out, window);
  append_u32(out, parent);
  append_u16(out, static_cast<std::uint16_t>(x));
  append_u16(out, static_cast<std::uint16_t>(y));
  append_u16(out, static_cast<std::uint16_t>(width));
  append_u16(out, static_cast<std::uint16_t>(height));
  append_u16(out, 0);  // border width
  append_u16(out, 1);  // InputOutput
  append_u32(out, 0);  // visual = CopyFromParent
  append_u32(out, 0x00000002);  // value mask: CWBackPixel
  append_u32(out, background_pixel);
  finish_request(out);
  return out;
}

std::string map_window_request(std::uint16_t sequence, std::uint32_t window) {
  std::string out = request_head(kOpMapWindow, 0, sequence);
  append_u32(out, window);
  finish_request(out);
  return out;
}

std::string select_input_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t event_mask) {
  std::string out = request_head(kOpSelectInput, 0, sequence);
  append_u32(out, window);
  append_u32(out, event_mask);
  finish_request(out);
  return out;
}

std::string create_gc_request(std::uint16_t sequence, std::uint32_t gc, std::uint32_t drawable,
                              std::uint32_t foreground) {
  std::string out = request_head(kOpCreateGC, 0, sequence);
  append_u32(out, gc);
  append_u32(out, drawable);
  append_u32(out, 0x00000004);  // value mask: GCForeground
  append_u32(out, foreground);
  finish_request(out);
  return out;
}

std::string change_property_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t property,
                                    std::uint32_t type, std::string_view data) {
  std::string out = request_head(kOpChangeProperty, 0, sequence);  // mode = Replace
  append_u32(out, window);
  append_u32(out, property);
  append_u32(out, type);
  out.push_back(8);  // format
  out.push_back('\0');
  append_u16(out, 0);
  append_u32(out, static_cast<std::uint32_t>(data.size()));
  out.append(data);
  finish_request(out);
  return out;
}

std::string change_property32_request(std::uint16_t sequence, std::uint32_t window, std::uint32_t property,
                                      std::string_view data) {
  std::string out = request_head(kOpChangeProperty, 0, sequence);  // mode = Replace
  append_u32(out, window);
  append_u32(out, property);
  append_u32(out, kAtomAtom);  // type = ATOM
  out.push_back(32);           // format
  out.push_back('\0');
  append_u16(out, 0);
  append_u32(out, static_cast<std::uint32_t>(data.size() / 4));
  out.append(data);
  finish_request(out);
  return out;
}

std::string set_input_focus_request(std::uint16_t sequence, std::uint32_t window) {
  std::string out = request_head(kOpSetInputFocus, 1, sequence);  // revert to PointerRoot
  append_u32(out, window);
  append_u32(out, 0);  // CurrentTime
  finish_request(out);
  return out;
}

std::string get_keyboard_mapping_request(std::uint16_t sequence, std::uint8_t first_keycode, std::uint8_t count) {
  std::string out = request_head(kOpGetKeyboardMapping, first_keycode, sequence);
  out.push_back(static_cast<char>(count));
  out.push_back('\0');
  append_u16(out, 0);
  finish_request(out);
  return out;
}

std::string put_image_request(std::uint16_t sequence, std::uint32_t drawable, std::uint32_t gc, int width, int height,
                              std::uint8_t depth, std::string_view pixels) {
  std::string out = request_head(kOpPutImage, 2, sequence);  // format = ZPixmap
  append_u32(out, drawable);
  append_u32(out, gc);
  append_u16(out, static_cast<std::uint16_t>(width));
  append_u16(out, static_cast<std::uint16_t>(height));
  append_u16(out, 0);  // dst-x
  append_u16(out, 0);  // dst-y
  out.push_back('\0');  // left pad
  out.push_back(static_cast<char>(depth));
  append_u16(out, 0);
  out.append(pixels);
  finish_request(out);
  return out;
}

Event decode_event(const std::uint8_t* packet) {
  Event event;
  const std::uint8_t code = packet[0] & 0x7F;
  switch (code) {
    case 12:  // Expose
      event.type = EventType::Expose;
      break;
    case 22:  // ConfigureNotify
      event.type = EventType::Configure;
      event.width = static_cast<int>(packet[20] | (packet[21] << 8));
      event.height = static_cast<int>(packet[22] | (packet[23] << 8));
      break;
    case 4:  // ButtonPress
      event.type = EventType::ButtonPress;
      event.button = packet[1];
      event.x = static_cast<int>(static_cast<std::int16_t>(packet[24] | (packet[25] << 8)));
      event.y = static_cast<int>(static_cast<std::int16_t>(packet[26] | (packet[27] << 8)));
      break;
    case 6:  // MotionNotify
      event.type = EventType::Motion;
      event.x = static_cast<int>(static_cast<std::int16_t>(packet[24] | (packet[25] << 8)));
      event.y = static_cast<int>(static_cast<std::int16_t>(packet[26] | (packet[27] << 8)));
      break;
    case 2:  // KeyPress
      event.type = EventType::KeyPress;
      event.keysym = 0;  // filled in by Window::poll (needs the keymap)
      event.button = packet[1];  // keycode
      break;
    default:
      break;
  }
  return event;
}

}  // namespace proto

namespace {

std::string display_socket_path(std::string_view display, int& display_number) {
  display_number = -1;
  if (display.empty()) return {};
  const std::size_t colon = display.rfind(':');
  if (colon == std::string_view::npos) return {};
  std::string_view rest = display.substr(colon + 1);
  const std::size_t dot = rest.find('.');
  if (dot != std::string_view::npos) rest = rest.substr(0, dot);
  if (rest.empty()) return {};
  display_number = std::atoi(std::string(rest).c_str());
  if (display_number < 0) return {};
  return "/tmp/.X11-unix/X" + std::to_string(display_number);
}

/// Reads an Xauthority file and returns the MIT-MAGIC-COOKIE-1 for `number`.
bool read_xauthority(int number, std::string& cookie) {
  const char* env = std::getenv("XAUTHORITY");
  std::string path;
  if (env != nullptr && *env != '\0') {
    path = env;
  } else {
    const char* home = std::getenv("HOME");
    if (home == nullptr) return false;
    path = std::string(home) + "/.Xauthority";
  }
  std::ifstream file{path, std::ios::binary};
  if (!file) return false;
  std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  std::size_t position = 0;
  auto read_u16 = [&](std::uint16_t& value) {
    if (position + 2 > data.size()) return false;
    value = static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[position]) |
                                       (static_cast<std::uint8_t>(data[position + 1]) << 8));
    position += 2;
    return true;
  };
  while (position + 2 <= data.size()) {
    std::uint16_t family = 0;
    if (!read_u16(family)) break;
    std::string address;
    std::string number_text;
    std::string name;
    std::string data_bytes;
    auto read_field = [&](std::string& field) {
      std::uint16_t length = 0;
      if (!read_u16(length)) return false;
      if (position + length > data.size()) return false;
      field.assign(data, position, length);
      position += length;
      return true;
    };
    if (!read_field(address) || !read_field(number_text) || !read_field(name) || !read_field(data_bytes)) break;
    if (name != "MIT-MAGIC-COOKIE-1") continue;
    if (!number_text.empty() && std::atoi(number_text.c_str()) != number) continue;
    cookie = data_bytes;
    return true;
  }
  return false;
}

std::uint32_t read_u32_at(const std::string& data, std::size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3])) << 24);
}

Error x11_error(std::string_view message) { return Error::make("shell.x11", message); }

}  // namespace

bool display_available() {
  const char* display = std::getenv("DISPLAY");
  if (display == nullptr || *display == '\0') return false;
  int number = -1;
  const std::string path = display_socket_path(display, number);
  if (path.empty()) return false;
  return ::access(path.c_str(), F_OK) == 0;
}

Window::~Window() { close(); }

Result<std::unique_ptr<Window>> Window::open(int width, int height, std::string_view title,
                                             std::string_view display_name) {
  std::unique_ptr<Window> window(new Window());
  window->width_ = width;
  window->height_ = height;

  std::string display;
  if (!display_name.empty()) {
    display = std::string(display_name);
  } else if (const char* env = std::getenv("DISPLAY"); env != nullptr) {
    display = env;
  }
  if (display.empty()) {
    return Result<std::unique_ptr<Window>>::failure(x11_error("DISPLAY is not set"));
  }

  Status status = window->handshake(display);
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  const std::uint32_t window_id = window->root_ + 2;
  const std::uint32_t gc_id = window->root_ + 4;

  status = window->write_all(proto::create_window_request(window->sequence_++, window_id, window->root_, 0, 0, width,
                                                          height, window->black_pixel_));
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());
  window->window_ = window_id;

  status = window->write_all(proto::change_property_request(window->sequence_++, window_id, proto::kAtomWmName,
                                                            proto::kAtomString, title));
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  std::uint32_t wm_delete = 0;
  if (window->intern_atom("WM_DELETE_WINDOW", wm_delete).is_ok()) {
    window->wm_delete_window_ = wm_delete;
    std::string protocols;
    proto::append_u32(protocols, wm_delete);
    std::string request =
        proto::change_property32_request(window->sequence_++, window_id, proto::kAtomWmProtocols, protocols);
    status = window->write_all(request);
    if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());
  }

  status = window->write_all(proto::create_gc_request(window->sequence_++, gc_id, window_id, window->black_pixel_));
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());
  window->gc_ = gc_id;

  const std::uint32_t mask = proto::kEventExposure | proto::kEventStructureNotify | proto::kEventButtonPress |
                             proto::kEventButtonRelease | proto::kEventPointerMotion | proto::kEventKeyPress;
  status = window->write_all(proto::select_input_request(window->sequence_++, window_id, mask));
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  status = window->write_all(proto::map_window_request(window->sequence_++, window_id));
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());
  status = window->write_all(proto::set_input_focus_request(window->sequence_++, window_id));
  if (status.is_error()) return Result<std::unique_ptr<Window>>::failure(status.error());

  (void)window->keyboard_mapping();  // best effort: scrolling works without it
  return Result<std::unique_ptr<Window>>::success(std::move(window));
}

Status Window::handshake(std::string_view display_name) {
  int number = -1;
  const std::string socket_path = display_socket_path(display_name, number);
  if (socket_path.empty()) return Status::failure(x11_error("unsupported DISPLAY '" + std::string(display_name) + "'"));

  fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_ < 0) return Status::failure(x11_error("cannot create a socket"));

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message = "cannot connect to " + socket_path + ": " + std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return Status::failure(x11_error(message));
  }

  std::string cookie;
  const bool have_cookie = read_xauthority(number, cookie);
  const std::string request =
      proto::setup_request(have_cookie ? "MIT-MAGIC-COOKIE-1" : std::string_view{}, cookie);
  Status status = write_all(request);
  if (status.is_error()) return status;

  std::uint8_t header[8] = {0};
  if (!read_exact(header, sizeof(header), 5000)) return Status::failure(x11_error("no setup reply from the server"));
  const std::uint8_t reply_status = header[0];
  const auto extra = static_cast<std::size_t>(header[6] | (header[7] << 8)) * 4;
  std::string body(extra, '\0');
  if (extra > 0 && !read_exact(body.data(), extra, 5000)) {
    return Status::failure(x11_error("truncated setup reply"));
  }
  if (reply_status != 1) {
    const auto reason_length = static_cast<std::size_t>(header[1]);
    const std::string reason = body.substr(0, std::min(reason_length, body.size()));
    return Status::failure(x11_error("server refused the connection: " + reason));
  }

  // Fixed part of the setup reply body.
  const std::size_t vendor_length = body.size() >= 16
                                        ? static_cast<std::size_t>(static_cast<std::uint8_t>(body[16]) |
                                                                   (static_cast<std::uint8_t>(body[17]) << 8))
                                        : 0;
  // Fixed part of the setup body is 32 bytes: release(4) id-base(4) id-mask(4) motion(4)
  // vendor_len(2) max_request(2) roots(1) formats(1) byte-orders(2) scanline(4) keycodes(2) pad(4).
  std::size_t position = 32 + ((vendor_length + 3) / 4) * 4;
  const std::size_t formats = body.size() >= 22 ? static_cast<std::uint8_t>(body[21]) : 0;
  position += formats * 8;
  if (position + 40 > body.size()) return Status::failure(x11_error("malformed setup reply"));

  root_ = read_u32_at(body, position);
  black_pixel_ = read_u32_at(body, position + 12);
  depth_ = static_cast<std::uint8_t>(body[position + 38]);
  min_keycode_ = static_cast<std::uint8_t>(body[29]);
  if (root_ == 0) return Status::failure(x11_error("root window is not available"));
  return Status::success();
}

Status Window::write_all(std::string_view bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t result = ::send(fd_, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
    if (result <= 0) {
      if (result < 0 && errno == EINTR) continue;
      return Status::failure(x11_error("write to the X server failed"));
    }
    written += static_cast<std::size_t>(result);
  }
  return Status::success();
}

bool Window::read_exact(void* buffer, std::size_t length, int timeout_ms) {
  auto* out = static_cast<std::uint8_t*>(buffer);
  std::size_t received = 0;
  while (received < length) {
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready <= 0) return false;
    const ssize_t result = ::recv(fd_, out + received, length - received, 0);
    if (result <= 0) {
      if (result < 0 && errno == EINTR) continue;
      return false;
    }
    received += static_cast<std::size_t>(result);
  }
  return true;
}

Status Window::intern_atom(std::string_view name, std::uint32_t& atom) {
  const std::uint16_t sequence = sequence_++;
  Status status = write_all(proto::intern_atom_request(sequence, name));
  if (status.is_error()) return status;

  for (int attempt = 0; attempt < 64; ++attempt) {
    std::uint8_t packet[32] = {0};
    if (!read_exact(packet, sizeof(packet), 5000)) return Status::failure(x11_error("no InternAtom reply"));
    if (packet[0] != 1) continue;  // not a reply: an event arrived first
    const auto reply_sequence = static_cast<std::uint16_t>(packet[2] | (packet[3] << 8));
    if (reply_sequence != sequence) continue;
    atom = static_cast<std::uint32_t>(packet[8]) | (static_cast<std::uint32_t>(packet[9]) << 8) |
           (static_cast<std::uint32_t>(packet[10]) << 16) | (static_cast<std::uint32_t>(packet[11]) << 24);
    return Status::success();
  }
  return Status::failure(x11_error("InternAtom reply was lost"));
}

Status Window::keyboard_mapping() {
  const std::uint8_t first = min_keycode_;
  const std::uint8_t count = static_cast<std::uint8_t>(255 - min_keycode_);
  const std::uint16_t sequence = sequence_++;
  Status status = write_all(proto::get_keyboard_mapping_request(sequence, first, count));
  if (status.is_error()) return status;

  std::uint8_t packet[32] = {0};
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (!read_exact(packet, sizeof(packet), 5000)) return Status::failure(x11_error("no keyboard mapping reply"));
    if (packet[0] != 1) continue;
    if (static_cast<std::uint16_t>(packet[2] | (packet[3] << 8)) != sequence) continue;

    keysyms_per_keycode_ = packet[1];
    const auto units = static_cast<std::size_t>(packet[4] | (packet[5] << 8)) * 4;
    std::string body(units, '\0');
    if (!read_exact(body.data(), units, 5000)) return Status::failure(x11_error("truncated keymap"));
    const std::size_t total = units / 4;
    keysyms_.resize(total);
    for (std::size_t i = 0; i < total; ++i) keysyms_[i] = read_u32_at(body, i * 4);
    return Status::success();
  }
  return Status::failure(x11_error("GetKeyboardMapping reply was lost"));
}

std::uint32_t Window::keysym_for(std::uint8_t keycode) const {
  if (keysyms_per_keycode_ == 0 || keycode < min_keycode_) return 0;
  const std::size_t index = static_cast<std::size_t>(keycode - min_keycode_) * keysyms_per_keycode_;
  if (index >= keysyms_.size()) return 0;
  return keysyms_[index];
}

Status Window::present(const paint::Canvas& canvas) {
  if (fd_ < 0 || window_ == 0) return Status::failure(x11_error("window is closed"));
  const int width = canvas.width();
  const int height = canvas.height();

  // X11 ZPixmap on a little-endian server wants BGRX; our canvas is RGBA.
  std::string pixels;
  pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const css::Color pixel = canvas.pixel_at(x, y);
      pixels.push_back(static_cast<char>(pixel.b));
      pixels.push_back(static_cast<char>(pixel.g));
      pixels.push_back(static_cast<char>(pixel.r));
      pixels.push_back('\0');
    }
  }
  return write_all(proto::put_image_request(sequence_++, window_, gc_, width, height, depth_, pixels));
}

Event Window::poll(int timeout_ms) {
  if (fd_ < 0) return Event{};
  pollfd descriptor{fd_, POLLIN, 0};
  const int ready = ::poll(&descriptor, 1, timeout_ms);
  if (ready <= 0) return Event{};

  std::uint8_t packet[32] = {0};
  if (!read_exact(packet, sizeof(packet), 0)) return Event{};

  Event event = proto::decode_event(packet);
  switch (event.type) {
    case EventType::KeyPress:
      event.keysym = keysym_for(static_cast<std::uint8_t>(event.button));
      event.button = 0;
      break;
    case EventType::Configure:
      if (event.width > 0 && event.height > 0) {
        width_ = event.width;
        height_ = event.height;
      }
      break;
    default:
      break;
  }

  // A WM_DELETE_WINDOW ClientMessage closes the window.
  if ((packet[0] & 0x7F) == 33 && wm_delete_window_ != 0) {
    const std::uint32_t message_type = read_u32_at(std::string(reinterpret_cast<char*>(packet), 32), 8);
    const std::uint32_t first_data = read_u32_at(std::string(reinterpret_cast<char*>(packet), 32), 12);
    if (message_type == proto::kAtomWmProtocols && first_data == wm_delete_window_) {
      event.type = EventType::Close;
    }
  }
  return event;
}

void Window::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  window_ = 0;
  gc_ = 0;
}

}  // namespace bonk::shell::x11
