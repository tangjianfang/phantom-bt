#pragma once
// GATT 属性数据库
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace pb::ble {

// 属性类型
constexpr uint16_t T_PRIMARY_SVC = 0x2800;
constexpr uint16_t T_CHAR_DECL = 0x2803;
constexpr uint16_t T_CCC = 0x2902;
constexpr uint16_t T_REPORT_REF = 0x2908;
// 服务 UUID
constexpr uint16_t U_GAP = 0x1800;
constexpr uint16_t U_HID = 0x1812;
constexpr uint16_t U_BATTERY = 0x180F;
constexpr uint16_t U_DEVICE_INFO = 0x180A;
// 特征 UUID
constexpr uint16_t U_DEVICE_NAME = 0x2A00;
constexpr uint16_t U_APPEARANCE = 0x2A01;
constexpr uint16_t U_HID_INFO = 0x2A4A;
constexpr uint16_t U_REPORT_MAP = 0x2A4B;
constexpr uint16_t U_HID_CTRL_POINT = 0x2A4C;
constexpr uint16_t U_PROTOCOL_MODE = 0x2A4E;
constexpr uint16_t U_REPORT = 0x2A4D;
constexpr uint16_t U_BATTERY_LEVEL = 0x2A19;

// ATT 错误码
constexpr uint8_t E_INVALID_HANDLE = 0x01;
constexpr uint8_t E_READ_NOT_PERMITTED = 0x02;
constexpr uint8_t E_WRITE_NOT_PERMITTED = 0x03;
constexpr uint8_t E_INVALID_PDU = 0x04;
constexpr uint8_t E_INSUFFICIENT_AUTHN = 0x05;
constexpr uint8_t E_UNSUPPORTED_REQ = 0x06;
constexpr uint8_t E_INVALID_OFFSET = 0x07;
constexpr uint8_t E_ATTR_NOT_FOUND = 0x0A;
constexpr uint8_t E_ATTR_NOT_LONG = 0x0B;
constexpr uint8_t E_INSUFFICIENT_RESOURCES = 0x11;

// 特征属性位
constexpr uint8_t P_READ = 0x02, P_WRITE_NO_RSP = 0x04, P_WRITE = 0x08, P_NOTIFY = 0x10,
                  P_INDICATE = 0x20;

struct GattAttr {
  uint16_t handle = 0;
  uint16_t type = 0;          // 0x2800/0x2803/0x2902/0x2908/0x2Axx 值类型
  uint8_t props = 0;          // 仅 char decl
  uint16_t value_type = 0;    // char decl 指向的值句柄类型
  std::vector<uint8_t> value; // 服务声明值 = 服务 UUID LE
  bool readable = true;
  bool writable = false;
};

class GattDb {
 public:
  static GattDb build(std::vector<GattAttr> attrs);  // 校验句柄递增唯一
  const GattAttr* by_handle(uint16_t h) const;
  // 服务区间：svc → (start, end)
  std::vector<std::pair<uint16_t, uint16_t>> service_ranges() const;
  const GattAttr* find_value_by_type(uint16_t type, uint16_t after = 0) const;
  // 读：返回 (数据, 错误码 0=ok)
  std::pair<std::vector<uint8_t>, uint8_t> read_value(uint16_t h, uint16_t offset) const;
  uint8_t write_value(uint16_t h, const std::vector<uint8_t>& v);
  void set_value(uint16_t h, const std::vector<uint8_t>& v);  // 通知前更新
  // FindInformation：handle→uuid16 短格式对
  std::vector<std::pair<uint16_t, uint16_t>> descriptors_in_range(uint16_t start, uint16_t end) const;
  uint16_t mtu = 23;

 private:
  std::vector<GattAttr> attrs_;                       // 句柄升序
  std::map<uint16_t, size_t> index_;                   // handle → attrs_ 下标
};

}  // namespace pb::ble
