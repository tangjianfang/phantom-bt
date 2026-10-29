#pragma once
// 标准服务集：GAP + HID(HOGP) + Battery（固定句柄布局，见 plan Task 22）
#include <cstdint>
#include <string>
#include <vector>

#include "ble/gatt_db.h"

namespace pb::ble {

struct ServiceSet {
  std::string device_name = "PhantomKb";
  uint16_t appearance = 0x03C1;  // Keyboard
  std::vector<uint8_t> report_map;  // HID 报告映射（template_ble_keyboard_reportmap）
  uint8_t battery_level = 100;
};

// 句柄布局（0x0001..0x001A）：
//  0001 GAP svc | 0002/03 DeviceName | 0004/05 Appearance
//  0006 HID svc | 0007/08 HID Info | 0009/0A ReportMap | 000B/0C CtrlPoint |
//  000D/0E ProtocolMode | 000F/10/11/12 Report#1(input,ReportRef+CCC) |
//  0013/14/15 Report#2(output,ReportRef) | 0016 Battery svc | 0017/18 BatteryLevel | 0019 CCC
std::vector<GattAttr> build_keyboard_attrs(const ServiceSet& s);

// 便捷句柄常量
constexpr uint16_t H_DEVICE_NAME = 0x0003, H_APPEARANCE = 0x0005, H_HID_INFO = 0x0008,
                   H_REPORT_MAP = 0x000A, H_CTRL_POINT = 0x000C, H_PROTOCOL_MODE = 0x000E,
                   H_REPORT1_VALUE = 0x0010, H_REPORT1_REF = 0x0011, H_REPORT1_CCC = 0x0012,
                   H_REPORT2_VALUE = 0x0014, H_REPORT2_REF = 0x0015, H_BATTERY_VALUE = 0x0018,
                   H_BATTERY_CCC = 0x0019;

}  // namespace pb::ble
