#include "test/test_framework.h"
#include "usb/descriptors.h"

using namespace pb;
using namespace pb::usb;

PB_TEST(desc_device_minimal) {
  ConfigParams c;
  c.vid = 0x1234;
  c.pid = 0x5678;
  c.product = "phantom gamepad";
  auto d = build_device_descriptor(c);
  PB_CHECK_EQ(d.size(), 18u);
  PB_CHECK_EQ(d[0], 18u);        // bLength
  PB_CHECK_EQ(d[1], 1u);         // DT_DEVICE
  PB_CHECK_EQ(d[2], 0x00);       // bcdUSB LE = 0x0200
  PB_CHECK_EQ(d[3], 0x02);
  PB_CHECK_EQ(d[4], 0u);         // bDeviceClass
  PB_CHECK_EQ(d[5], 0u);
  PB_CHECK_EQ(d[6], 0u);
  PB_CHECK_EQ(d[7], 8u);         // bMaxPacketSize0
  PB_CHECK_EQ(d[8], 0x34);       // idVendor LE
  PB_CHECK_EQ(d[9], 0x12);
  PB_CHECK_EQ(d[10], 0x78);      // idProduct LE
  PB_CHECK_EQ(d[11], 0x56);
  PB_CHECK_EQ(d[12], 0x00);      // bcdDevice 0x0100 LE
  PB_CHECK_EQ(d[13], 0x01);
  PB_CHECK_EQ(d[14], 1u);        // iManufacturer
  PB_CHECK_EQ(d[15], 2u);        // iProduct
  PB_CHECK_EQ(d[16], 3u);        // iSerial
  PB_CHECK_EQ(d[17], 1u);        // bNumConfigurations
}

PB_TEST(desc_device_class_e0) {
  ConfigParams c;
  c.device_class = 0xE0;
  c.device_subclass = 0x01;
  c.device_protocol = 0x01;
  c.bcd_usb = 0x0110;
  c.max_packet0 = 64;
  auto d = build_device_descriptor(c);
  PB_CHECK_EQ(d[4], 0xE0);  // bDeviceClass
  PB_CHECK_EQ(d[5], 0x01);
  PB_CHECK_EQ(d[6], 0x01);
  PB_CHECK_EQ(d[7], 64u);   // bMaxPacketSize0
  PB_CHECK_EQ(d[2], 0x10);  // bcdUSB 0x0110 LE
  PB_CHECK_EQ(d[3], 0x01);
}

PB_TEST(desc_config_hid_interrupt) {
  ConfigParams c;
  c.vid = 1;
  c.pid = 2;
  c.product = "x";
  InterfaceDef intf;
  intf.number = 0;
  intf.alt_setting = 0;
  intf.iclass = 3;  // HID
  intf.isubclass = 0;
  intf.iprotocol = 0;
  intf.hid = HidClassDescriptor{.report_length = 35};
  intf.endpoints = {{0x81, 3, 8, 10}};
  c.interfaces = {intf};
  auto cfg = build_config_descriptor(c);
  // 9(config) + 9(iface) + 9(hid) + 7(ep) = 34
  PB_CHECK_EQ(cfg.size(), 34u);
  PB_CHECK_EQ(cfg[0], 9u);
  PB_CHECK_EQ(cfg[1], 2u);          // DT_CONFIG
  PB_CHECK_EQ(cfg[2], 34u);         // wTotalLength LE
  PB_CHECK_EQ(cfg[3], 0u);
  PB_CHECK_EQ(cfg[4], 1u);          // bNumInterfaces
  PB_CHECK_EQ(cfg[5], 1u);          // bConfigurationValue
  PB_CHECK_EQ(cfg[6], 0u);          // iConfiguration
  PB_CHECK_EQ(cfg[7], 0xA0u);       // attr: 0x80|remote_wakeup
  PB_CHECK_EQ(cfg[8], 50u);         // bMaxPower
  PB_CHECK_EQ(cfg[9], 9u);          // iface bLength
  PB_CHECK_EQ(cfg[10], 4u);         // DT_INTERFACE
  PB_CHECK_EQ(cfg[11], 0u);         // bInterfaceNumber
  PB_CHECK_EQ(cfg[14], 3u);         // class HID
  PB_CHECK_EQ(cfg[18], 9u);         // hid bLength
  PB_CHECK_EQ(cfg[19], 0x21u);      // DT_HID
  PB_CHECK_EQ(cfg[20], 0x11);       // bcdHID 0x0111 LE
  PB_CHECK_EQ(cfg[21], 0x01);
  PB_CHECK_EQ(cfg[25], 35u);        // report len LE
  PB_CHECK_EQ(cfg[26], 0u);
  PB_CHECK_EQ(cfg[27], 7u);         // ep bLength
  PB_CHECK_EQ(cfg[28], 5u);         // DT_ENDPOINT
  PB_CHECK_EQ(cfg[29], 0x81u);
  PB_CHECK_EQ(cfg[30], 3u);
  PB_CHECK_EQ(cfg[31], 8u);
  PB_CHECK_EQ(cfg[33], 10u);
}

PB_TEST(desc_config_bluetooth_two_interfaces) {
  ConfigParams c;
  c.vid = 0x0B00;
  c.pid = 0x0001;
  c.product = "phantom bt";
  InterfaceDef i0;
  i0.number = 0;
  i0.iclass = 0xE0;
  i0.isubclass = 1;
  i0.iprotocol = 1;
  i0.endpoints = {{0x81, 3, 16, 1}, {0x02, 2, 64, 0}, {0x82, 2, 64, 0}};
  InterfaceDef i1;
  i1.number = 1;
  i1.iclass = 0xE0;
  i1.isubclass = 1;
  i1.iprotocol = 1;
  c.interfaces = {i0, i1};
  auto cfg = build_config_descriptor(c);
  PB_CHECK_EQ(cfg.size(), 9u + 9u + 7 * 3 + 9u);  // 48
  PB_CHECK_EQ(cfg[2], 48u);
  PB_CHECK_EQ(cfg[3], 0u);
  PB_CHECK_EQ(cfg[4], 2u);       // bNumInterfaces=2
  PB_CHECK_EQ(cfg[14], 0xE0u);   // iface0 class（iface@9: class@9+5）
  PB_CHECK_EQ(cfg[11], 0u);      // iface0 number
  // iface0: 9+7*3=30 → iface1 起点 9+30=39
  PB_CHECK_EQ(cfg[39], 9u);      // bLength
  PB_CHECK_EQ(cfg[40], 4u);      // DT_INTERFACE
  PB_CHECK_EQ(cfg[41], 1u);      // bInterfaceNumber=1
  PB_CHECK_EQ(cfg[43], 0u);      // bNumEndpoints=0（空 SCO alt0）
  PB_CHECK_EQ(cfg[44], 0xE0u);   // iface1 class
}

PB_TEST(desc_string_utf16le) {
  auto s = build_string_descriptor("AB");
  PB_CHECK_EQ(s.size(), 6u);
  PB_CHECK_EQ(s[0], 6u);
  PB_CHECK_EQ(s[1], 3u);
  PB_CHECK_EQ(s[2], 'A');
  PB_CHECK_EQ(s[3], 0);
  PB_CHECK_EQ(s[4], 'B');
  PB_CHECK_EQ(s[5], 0);
}

PB_TEST(desc_string_zero_langid) {
  auto s = build_string_descriptor_zero();
  PB_CHECK_EQ(s.size(), 4u);
  PB_CHECK_EQ(s[0], 4u);
  PB_CHECK_EQ(s[1], 3u);
  PB_CHECK_EQ(s[2], 0x09);
  PB_CHECK_EQ(s[3], 0x04);
}

PB_TEST(desc_string_utf8_chinese) {
  auto s = build_string_descriptor("中文");
  // 每个 BMP 汉字 → 2 字节 UTF-16LE
  PB_CHECK_EQ(s.size(), 2u + 4u);
  // '中' U+4E2D → 2D 4E
  PB_CHECK_EQ(s[2], 0x2D);
  PB_CHECK_EQ(s[3], 0x4E);
}
