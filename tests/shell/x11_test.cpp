#include <cstdint>
#include <string>
#include <vector>

#include "bonk/shell/x11.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

/// Little-endian read of the 16-bit length field of an X11 request.
std::uint16_t request_length(const std::string& request) {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(request[2]) |
                                    (static_cast<std::uint8_t>(request[3]) << 8));
}

std::uint32_t read_u32(const std::string& data, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3])) << 24);
}

}  // namespace

BONK_TEST(x11, setup_request_layout) {
  const std::string request = shell::x11::proto::setup_request("MIT-MAGIC-COOKIE-1", "0123456789abcdef");
  CHECK_EQ(request.size() % 4, std::size_t{0});
  CHECK_EQ(request[0], 'l');  // little-endian
  CHECK_EQ(request[2], '\x0B');  // protocol major 11
  CHECK_EQ(static_cast<std::uint8_t>(request[6]), 18);  // auth name length
  CHECK_EQ(static_cast<std::uint8_t>(request[8]), 16);  // auth data length
  CHECK_EQ(request.substr(12, 18), std::string("MIT-MAGIC-COOKIE-1"));
}

BONK_TEST(x11, create_window_request_is_well_formed) {
  const std::string request = shell::x11::proto::create_window_request(7, 0x200002, 0x500, 10, 20, 800, 600, 0xFF);
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(request[0])), 1);   // CreateWindow
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(request[1])), 24);  // depth 24
  CHECK_EQ(static_cast<std::size_t>(request_length(request)) * 4 + 4, request.size());
  CHECK_EQ(read_u32(request, 4), 0x200002u);  // window id
  CHECK_EQ(read_u32(request, 8), 0x500u);     // parent
  CHECK_EQ(static_cast<std::uint8_t>(request[12]), 10);  // x
  CHECK_EQ(static_cast<std::uint8_t>(request[16]), 800 & 0xFF);  // width low byte
  CHECK_EQ(read_u32(request, request.size() - 4), 0xFFu);  // background pixel value
}

BONK_TEST(x11, put_image_request_carries_pixels) {
  const std::string pixels(4 * 2 * 2, '\x7F');
  const std::string request = shell::x11::proto::put_image_request(3, 0x400001, 0x400002, 2, 2, 24, pixels);
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(request[0])), 72);  // PutImage
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(request[1])), 2);   // ZPixmap
  CHECK_EQ(read_u32(request, 4), 0x400001u);  // drawable
  CHECK_EQ(read_u32(request, 8), 0x400002u);  // gc
  CHECK_EQ(static_cast<std::uint8_t>(request[12]), 2);  // width
  CHECK_EQ(static_cast<std::uint8_t>(request[21]), 24);  // depth
  CHECK(request.size() >= pixels.size() + 24);
  CHECK_EQ(request.compare(request.size() - pixels.size(), pixels.size(), pixels), 0);
}

BONK_TEST(x11, intern_atom_and_mapping_requests) {
  const std::string atom = shell::x11::proto::intern_atom_request(2, "WM_DELETE_WINDOW");
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(atom[0])), 16);
  CHECK_EQ(static_cast<std::uint8_t>(atom[4]), 16);  // name length
  CHECK_EQ(atom.substr(8, 16), std::string("WM_DELETE_WINDOW"));
  CHECK_EQ(atom.size() % 4, std::size_t{0});

  const std::string keymap = shell::x11::proto::get_keyboard_mapping_request(5, 8, 247);
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(keymap[0])), 101);
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(keymap[1])), 8);    // first keycode
  CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(keymap[4])), 247);  // count
}

BONK_TEST(x11, decodes_expose_and_configure_events) {
  std::vector<std::uint8_t> packet(32, 0);
  packet[0] = 12;  // Expose
  CHECK(shell::x11::proto::decode_event(packet.data()).type == shell::x11::EventType::Expose);

  packet.assign(32, 0);
  packet[0] = 22;  // ConfigureNotify
  packet[20] = 0x20;
  packet[21] = 0x03;  // width 800
  packet[22] = 0x58;
  packet[23] = 0x02;  // height 600
  const shell::x11::Event configure = shell::x11::proto::decode_event(packet.data());
  CHECK(configure.type == shell::x11::EventType::Configure);
  CHECK_EQ(configure.width, 800);
  CHECK_EQ(configure.height, 600);
}

BONK_TEST(x11, decodes_button_press_coordinates) {
  std::vector<std::uint8_t> packet(32, 0);
  packet[0] = 4;   // ButtonPress
  packet[1] = 1;   // left button
  packet[24] = 0xD2;
  packet[25] = 0x00;  // x = 210
  packet[26] = 0x2C;
  packet[27] = 0x01;  // y = 300
  const shell::x11::Event event = shell::x11::proto::decode_event(packet.data());
  CHECK(event.type == shell::x11::EventType::ButtonPress);
  CHECK_EQ(event.button, 1);
  CHECK_EQ(event.x, 210);
  CHECK_EQ(event.y, 300);
}

BONK_TEST(x11, ignores_unknown_events) {
  std::vector<std::uint8_t> packet(32, 0);
  packet[0] = 34;  // MappingNotify, not handled
  CHECK(shell::x11::proto::decode_event(packet.data()).type == shell::x11::EventType::None);
}

BONK_TEST(x11, display_probe_does_not_crash) {
  // In CI there is no display: the probe must report "not available" rather than throw.
  const bool available = shell::x11::display_available();
  CHECK(available == false || available == true);
}
