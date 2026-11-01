#include "test/test_framework.h"
#include "hid/report_descriptor.h"

#include <algorithm>

using namespace pb::hid;

// 快照更新（高优#3）：vendor 集合自带 Report ID(2) —— Report ID 为 global 项，
// 不显式声明会继承主集合 Report ID，违反 HID §6.2.1.7（29 字节，原 27）
static const uint8_t kVendorExpect[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
                                        0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,
                                        0x95, 0x40, 0x91, 0x02, 0x09, 0x03, 0x95, 0x40, 0x81,
                                        0x02, 0xC0};

PB_TEST(rdesc_vendor_template_exact) {
  auto d = template_vendor();
  PB_CHECK_EQ(d.size(), sizeof(kVendorExpect));
  PB_CHECK(std::equal(d.begin(), d.end(), kVendorExpect));
}

PB_TEST(rdesc_vendor_suffix_len_matches) {
  PB_CHECK_EQ(vendor_suffix_len(), sizeof(kVendorExpect));
}

PB_TEST(rdesc_gamepad_shape) {
  auto d = template_gamepad();
  PB_CHECK_EQ(d[0], 0x05);
  PB_CHECK_EQ(d[1], 0x01);
  PB_CHECK_EQ(d[3], 0x05);   // Usage (Game Pad)
  PB_CHECK_EQ(d[4], 0xA1);
  PB_CHECK_EQ(d.back(), 0xC0);
  PB_CHECK(std::count(d.begin(), d.end(), 0xC0) >= 3);  // physical + app + vendor
  // vendor 集合是后缀
  PB_CHECK_EQ(d.size() - vendor_suffix_len() + 0, d.size() - sizeof(kVendorExpect));
  PB_CHECK(std::equal(d.end() - sizeof(kVendorExpect), d.end(), kVendorExpect));
}

PB_TEST(rdesc_keyboard_has_vendor_suffix) {
  auto d = template_keyboard();
  PB_CHECK_EQ(d[0], 0x05);
  PB_CHECK_EQ(d[3], 0x06);  // Usage (Keyboard)
  PB_CHECK(std::equal(d.end() - sizeof(kVendorExpect), d.end(), kVendorExpect));
  // 含 report id 1
  bool has_report_id = false;
  for (size_t i = 0; i + 1 < d.size(); ++i)
    if (d[i] == 0x85 && d[i + 1] == 0x01) has_report_id = true;
  PB_CHECK(has_report_id);
}

PB_TEST(rdesc_mouse_shape) {
  auto d = template_mouse();
  PB_CHECK_EQ(d[3], 0x02);  // Usage (Mouse)
  PB_CHECK(std::equal(d.end() - sizeof(kVendorExpect), d.end(), kVendorExpect));
}

PB_TEST(rdesc_ble_reportmap_no_vendor) {
  auto d = template_ble_keyboard_reportmap();
  auto v = template_vendor();
  PB_CHECK(std::search(d.begin(), d.end(), v.begin(), v.end()) == d.end());
  PB_CHECK_EQ(d[3], 0x06);  // Usage (Keyboard)
  PB_CHECK_EQ(d.back(), 0xC0);
}
