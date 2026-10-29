#pragma once
// BLE 虚拟设备定义（JSON Profile → 空口广播源）
#include <cstdint>
#include <string>

#include "core/json.h"

namespace pb::ble {

struct BleDeviceDef {
  std::string kind;          // keyboard | beacon
  std::string name;
  uint64_t address = 0;      // 静态随机（C0:xx..），解析自 "C0:11:22:33:44:55"
  bool connectable = true;
  std::string hid_map;       // keyboard: boot_keyboard

  static BleDeviceDef from_json(const pb::json::Value& v);  // 抛错带字段名
  static BleDeviceDef load_file(const std::string& path);
  static uint64_t parse_address(std::string_view s);  // "C0:11:22:33:44:55" → u64（LSB first 值序）
  static std::string format_address(uint64_t addr);   // 逆操作
};

}  // namespace pb::ble
