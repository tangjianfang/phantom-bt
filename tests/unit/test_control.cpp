#include "test/test_framework.h"
#include "usb/control.h"

#include <array>

using namespace pb;
using namespace pb::usb;

static std::array<uint8_t, 8> make_setup8(uint8_t bm, uint8_t br, uint16_t wv, uint16_t wi, uint16_t wl) {
  return {bm, br, (uint8_t)(wv & 0xFF), (uint8_t)(wv >> 8), (uint8_t)(wi & 0xFF), (uint8_t)(wi >> 8),
          (uint8_t)(wl & 0xFF), (uint8_t)(wl >> 8)};
}

static DescriptorStore store() {
  DescriptorStore s;
  ConfigParams c;
  c.vid = 1;
  c.pid = 2;
  c.product = "t";
  InterfaceDef i;
  i.number = 0;
  i.iclass = 3;
  i.endpoints = {{0x81, 3, 8, 1}};
  c.interfaces = {i};
  s.device = build_device_descriptor(c);
  s.config = build_config_descriptor(c);
  s.strings[0] = build_string_descriptor_zero();
  s.strings[1] = build_string_descriptor("Phantom-BT");
  s.strings[2] = build_string_descriptor("t");
  s.strings[3] = build_string_descriptor("0001");
  s.report_descriptor = {0xC0};
  return s;
}

PB_TEST(control_parse_setup_le) {
  auto su = parse_setup(make_setup8(0x80, 6, 0x0100, 0, 18).data());
  PB_CHECK_EQ(su.bmRequestType, 0x80);
  PB_CHECK_EQ(su.bRequest, 6);
  PB_CHECK_EQ(su.wValue, 0x0100);
  PB_CHECK_EQ(su.wIndex, 0);
  PB_CHECK_EQ(su.wLength, 18);
}

PB_TEST(control_get_descriptor_device) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0100, 0, 18).data());
  PB_CHECK(descriptor_request(su, s, out));
  PB_CHECK_EQ(out.size(), 18u);
  PB_CHECK_EQ(out[0], 18u);
  PB_CHECK_EQ(out[1], 1u);
}

PB_TEST(control_get_descriptor_config_truncated_by_wlength) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0200, 0, 4).data());
  PB_CHECK(descriptor_request(su, s, out));
  PB_CHECK_EQ(out.size(), 4u);
  PB_CHECK_EQ(out[0], 9u);  // 仍以完整描述符开头
}

PB_TEST(control_get_descriptor_string_by_index) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0302, 0x0409, 255).data());
  PB_CHECK(descriptor_request(su, s, out));
  // 字符串 "t" → 4B 描述符
  PB_CHECK_EQ(out.size(), 4u);
  PB_CHECK_EQ(out[2], 't');
}

PB_TEST(control_get_descriptor_missing_string_falls_back) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0309, 0, 4).data());  // 无 index 9
  PB_CHECK(!descriptor_request(su, s, out));
}

PB_TEST(control_class_request_not_standard) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x81, 0x06, 0x0022, 0, 26).data());  // HID GET_DESCRIPTOR(report)
  PB_CHECK(!descriptor_request(su, s, out));  // 类请求交模型
}

PB_TEST(control_set_config_and_get_status) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x00, REQ_SET_CONFIGURATION, 1, 0, 0).data());
  PB_CHECK(descriptor_request(su, s, out));
  PB_CHECK(out.empty());
  auto gs = parse_setup(make_setup8(0x80, REQ_GET_STATUS, 0, 0, 2).data());
  PB_CHECK(descriptor_request(gs, s, out));
  PB_CHECK_EQ(out.size(), 2u);
}

PB_TEST(control_device_qualifier_falls_back_for_stall) {
  auto s = store();
  std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0600, 0, 8).data());
  PB_CHECK(!descriptor_request(su, s, out));
}
