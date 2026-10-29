#pragma once
// HID 报告描述符模板（每个 USB 模板附带 vendor-defined 集合，保证 WinRT HidDevice 可打开）
#include <cstdint>
#include <vector>

namespace pb::hid {

std::vector<uint8_t> template_gamepad();   // hat(4b)+16 按钮+X/Y/Z/RZ(8b×4)
std::vector<uint8_t> template_keyboard();  // boot 键盘 8B 输入 + 1B LED 输出
std::vector<uint8_t> template_mouse();     // 3 键 + 相对 XY(16b) + 滚轮(8b)
std::vector<uint8_t> template_vendor();    // 纯 vendor：64B 输入 + 64B 输出
std::vector<uint8_t> template_ble_keyboard_reportmap();  // BLE HOGP 用（无 vendor 集合）

// 返回主集合（不含 vendor 附集）长度：USB 模板 = 主 + vendor
size_t vendor_suffix_len();

}  // namespace pb::hid
