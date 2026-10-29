#pragma once
// HID Profile：模板 + JSON 覆盖（vid/pid/名称）
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace pb::hid {

struct HidProfile {
  std::string template_name;  // gamepad|keyboard|mouse|vendor
  uint16_t vid = 0, pid = 0;
  std::string product = "Phantom Device";
  std::string manufacturer = "Phantom-BT";
  std::string serial = "000000000001";
  std::optional<std::vector<uint8_t>> report_descriptor_override;  // hex 覆盖

  static HidProfile from_json(const pb::json::Value& v);  // 抛错带字段名
  static HidProfile load_file(const std::string& path);
};

// "0x1234" / "4660" → 0x1234；非法抛错
uint16_t parse_u16(std::string_view s);

}  // namespace pb::hid
