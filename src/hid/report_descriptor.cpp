#include "hid/report_descriptor.h"

#include <cstring>

namespace pb::hid {

// vendor-defined 顶层集合：UsagePage(0xFF00) Usage(0x01) App
//   Output 64B(usage 0x02) + Input 64B(usage 0x03)
// 自带 Report ID (2)：Report ID 为 global 项，跨集合持久 —— 不显式声明时
// 本集合字段会继承主集合的 Report ID（或全描述符无 ID），违反 HID §6.2.1.7
static const uint8_t kVendorColl[] = {
    0x06, 0x00, 0xFF,                          // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,                                // Usage (0x01)
    0xA1, 0x01,                                // Collection (Application)
    0x85, 0x02,                                //   Report ID (2)
    0x09, 0x02,                                //   Usage (0x02)
    0x15, 0x00,                                //   Logical Min 0
    0x26, 0xFF, 0x00,                          //   Logical Max 255
    0x75, 0x08,                                //   Report Size 8
    0x95, 0x40,                                //   Report Count 64
    0x91, 0x02,                                //   Output (Data,Var,Abs)
    0x09, 0x03,                                //   Usage (0x03)
    0x95, 0x40,                                //   Report Count 64
    0x81, 0x02,                                //   Input (Data,Var,Abs)
    0xC0,                                      // End Collection
};

// 键盘主集合（report id 1）：修饰键 8b + 常量 8b + 键码 6×8b 输入；LED 5b+3b 输出
static const uint8_t kKeyboardMain[] = {
    0x05, 0x01,                                // Usage Page (Generic Desktop)
    0x09, 0x06,                                // Usage (Keyboard)
    0xA1, 0x01,                                // Collection (Application)
    0x85, 0x01,                                //   Report ID (1)
    0x05, 0x07,                                //   Usage Page (Key Codes)
    0x19, 0xE0,                                //   Usage Min (224)
    0x29, 0xE7,                                //   Usage Max (231)
    0x15, 0x00,                                //   Logical Min (0)
    0x25, 0x01,                                //   Logical Max (1)
    0x75, 0x01,                                //   Report Size (1)
    0x95, 0x08,                                //   Report Count (8)
    0x81, 0x02,                                //   Input (Data,Var,Abs) 修饰键（HID 1.11 附录 B.1 canonical 形态）
    0x95, 0x01,                                //   Report Count (1)
    0x75, 0x08,                                //   Report Size (8)
    0x81, 0x01,                                //   Input (Const) 保留
    0x95, 0x06,                                //   Report Count (6)
    0x75, 0x08,                                //   Report Size (8)
    0x15, 0x00,                                //   Logical Min (0)
    0x26, 0xFF, 0x00,                          //   Logical Max (255)
    0x05, 0x07,                                //   Usage Page (Key Codes)
    0x19, 0x00,                                //   Usage Min (0)
    0x2A, 0xFF, 0x00,                          //   Usage Max (255)
    0x81, 0x00,                                //   Input (Data,Array,Abs) 键码
    0x05, 0x08,                                //   Usage Page (LEDs)
    0x09, 0x01,                                //   Usage (Num Lock)
    0x95, 0x05,                                //   Report Count (5)
    0x75, 0x01,                                //   Report Size (1)
    0x91, 0x02,                                //   Output (Data,Var,Abs)
    0x95, 0x01,                                //   Report Count (1)
    0x75, 0x03,                                //   Report Size (3)
    0x91, 0x01,                                //   Output (Const)
    0xC0,                                      // End Collection
};

static const uint8_t kMouseMain[] = {
    0x05, 0x01,                                // Usage Page (Generic Desktop)
    0x09, 0x02,                                // Usage (Mouse)
    0xA1, 0x01,                                // Collection (Application)
    0x09, 0x01,                                //   Usage (Pointer)
    0xA1, 0x00,                                //   Collection (Physical)
    0x85, 0x01,                                //     Report ID (1)
    0x05, 0x09,                                //     Usage Page (Buttons)
    0x19, 0x01,                                //     Usage Min (1)
    0x29, 0x03,                                //     Usage Max (3)
    0x15, 0x00,                                //     Logical Min (0)
    0x25, 0x01,                                //     Logical Max (1)
    0x95, 0x03,                                //     Report Count (3)
    0x75, 0x01,                                //     Report Size (1)
    0x81, 0x02,                                //     Input (Data,Var,Abs)
    0x95, 0x01,                                //     Report Count (1)
    0x75, 0x05,                                //     Report Size (5)
    0x81, 0x03,                                //     Input (Const,Var,Abs) 填充
    0x05, 0x01,                                //     Usage Page (Generic Desktop)
    0x09, 0x30,                                //     Usage (X)
    0x09, 0x31,                                //     Usage (Y)
    0x16, 0x01, 0x80,                          //     Logical Min (-32767)
    0x26, 0xFF, 0x7F,                          //     Logical Max (32767)
    0x75, 0x10,                                //     Report Size (16)
    0x95, 0x02,                                //     Report Count (2)
    0x81, 0x06,                                //     Input (Data,Var,Rel)
    0x09, 0x38,                                //     Usage (Wheel)
    0x15, 0x81,                                //     Logical Min (-127)
    0x25, 0x7F,                                //     Logical Max (127)
    0x75, 0x08,                                //     Report Size (8)
    0x95, 0x01,                                //     Report Count (1)
    0x81, 0x06,                                //     Input (Data,Var,Rel)
    0xC0,                                      //   End Collection (Physical)
    0xC0,                                      // End Collection (Application)
};

static const uint8_t kGamepadMain[] = {
    0x05, 0x01,                                // Usage Page (Generic Desktop)
    0x09, 0x05,                                // Usage (Game Pad)
    0xA1, 0x01,                                // Collection (Application)
    0x85, 0x01,                                //   Report ID (1)（拼接 vendor 集合后全描述符均带 ID）
    0xA1, 0x00,                                //   Collection (Physical)
    // hat 4b 须带 4b Const 填充补齐整字节（经典 XInput 形态）：否则报告总长 52bits
    // =6.5 字节，Windows HidUsb 解析后设备启动失败（problem code 10，真机实测）
    0x05, 0x01,                                //     Usage Page (Generic Desktop)
    0x09, 0x39,                                //     Usage (Hat switch)
    0x15, 0x00,                                //     Logical Min (0)
    0x25, 0x07,                                //     Logical Max (7)
    0x35, 0x00,                                //     Physical Min (0)
    0x46, 0x3B, 0x01,                          //     Physical Max (315)
    0x65, 0x14,                                //     Unit (Eng Rot: Degree)
    0x75, 0x04,                                //     Report Size (4)
    0x95, 0x01,                                //     Report Count (1)
    0x81, 0x42,                                //     Input (Data,Var,Abs,Null State)
    0x75, 0x04,                                //     Report Size (4)
    0x95, 0x01,                                //     Report Count (1)
    0x81, 0x01,                                //     Input (Const,Array,Abs) 填充至整字节
    0x05, 0x09,                                //     Usage Page (Buttons)
    0x19, 0x01,                                //     Usage Min (1)
    0x29, 0x10,                                //     Usage Max (16)
    0x15, 0x00,                                //     Logical Min (0)
    0x25, 0x01,                                //     Logical Max (1)
    0x75, 0x01,                                //     Report Size (1)
    0x95, 0x10,                                //     Report Count (16)
    0x81, 0x02,                                //     Input (Data,Var,Abs)
    0x05, 0x01,                                //     Usage Page (Generic Desktop)
    0x09, 0x30,                                //     Usage (X)
    0x09, 0x31,                                //     Usage (Y)
    0x09, 0x32,                                //     Usage (Z)
    0x09, 0x35,                                //     Usage (Rz)
    0x15, 0x00,                                //     Logical Min (0)
    0x26, 0xFF, 0x00,                          //     Logical Max (255)
    0x75, 0x08,                                //     Report Size (8)
    0x95, 0x04,                                //     Report Count (4)
    0x81, 0x02,                                //     Input (Data,Var,Abs)
    0xC0,                                      //   End Collection (Physical)
    0xC0,                                      // End Collection (Application)
};

static std::vector<uint8_t> concat(const uint8_t* a, size_t na, const uint8_t* b, size_t nb) {
  std::vector<uint8_t> out;
  out.reserve(na + nb);
  out.insert(out.end(), a, a + na);
  out.insert(out.end(), b, b + nb);
  return out;
}

size_t vendor_suffix_len() { return sizeof(kVendorColl); }

std::vector<uint8_t> template_vendor() {
  return std::vector<uint8_t>(kVendorColl, kVendorColl + sizeof(kVendorColl));
}

std::vector<uint8_t> template_keyboard() {
  return concat(kKeyboardMain, sizeof(kKeyboardMain), kVendorColl, sizeof(kVendorColl));
}

std::vector<uint8_t> template_mouse() {
  return concat(kMouseMain, sizeof(kMouseMain), kVendorColl, sizeof(kVendorColl));
}

std::vector<uint8_t> template_gamepad() {
  return concat(kGamepadMain, sizeof(kGamepadMain), kVendorColl, sizeof(kVendorColl));
}

std::vector<uint8_t> template_ble_keyboard_reportmap() {
  return std::vector<uint8_t>(kKeyboardMain, kKeyboardMain + sizeof(kKeyboardMain));
}

}  // namespace pb::hid
