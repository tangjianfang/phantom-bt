#include "ble/services.h"

#include "core/bytes.h"

namespace pb::ble {

namespace {
GattAttr svc(uint16_t h, uint16_t uuid) {
  GattAttr a;
  a.handle = h;
  a.type = T_PRIMARY_SVC;
  ByteWriter w;
  w.u16le(uuid);
  a.value = w.take();
  a.readable = true;
  a.writable = false;
  return a;
}
GattAttr decl(uint16_t h, uint8_t props, uint16_t value_type) {
  GattAttr a;
  a.handle = h;
  a.type = T_CHAR_DECL;
  a.props = props;
  a.value_type = value_type;
  a.readable = true;
  a.writable = false;
  return a;
}
GattAttr val(uint16_t h, uint16_t type, std::vector<uint8_t> v, bool read, bool write) {
  GattAttr a;
  a.handle = h;
  a.type = type;
  a.value = std::move(v);
  a.readable = read;
  a.writable = write;
  return a;
}
GattAttr desc(uint16_t h, uint16_t type, std::vector<uint8_t> v, bool write) {
  GattAttr a;
  a.handle = h;
  a.type = type;
  a.value = std::move(v);
  a.readable = true;
  a.writable = write;
  return a;
}
std::vector<uint8_t> u16v(uint16_t x) {
  ByteWriter w;
  w.u16le(x);
  return w.take();
}
}  // namespace

std::vector<GattAttr> build_keyboard_attrs(const ServiceSet& s) {
  std::vector<GattAttr> out;
  // GAP
  out.push_back(svc(0x0001, U_GAP));
  out.push_back(decl(0x0002, P_READ, U_DEVICE_NAME));
  out.push_back(val(0x0003, U_DEVICE_NAME, {s.device_name.begin(), s.device_name.end()}, true, false));
  out.push_back(decl(0x0004, P_READ, U_APPEARANCE));
  out.push_back(val(0x0005, U_APPEARANCE, u16v(s.appearance), true, false));
  // HID
  out.push_back(svc(0x0006, U_HID));
  out.push_back(decl(0x0007, P_READ, U_HID_INFO));
  out.push_back(val(0x0008, U_HID_INFO, {0x01, 0x01, 0x00, 0x02}, true, false));  // bcd 1.1, country 0, flags remote-wake
  out.push_back(decl(0x0009, P_READ, U_REPORT_MAP));
  out.push_back(val(0x000A, U_REPORT_MAP, s.report_map, true, false));
  out.push_back(decl(0x000B, P_WRITE_NO_RSP, U_HID_CTRL_POINT));
  out.push_back(val(0x000C, U_HID_CTRL_POINT, {}, false, true));
  out.push_back(decl(0x000D, P_READ | P_WRITE_NO_RSP, U_PROTOCOL_MODE));
  out.push_back(val(0x000E, U_PROTOCOL_MODE, {0x01}, true, true));  // report 模式
  out.push_back(decl(0x000F, P_READ | P_NOTIFY, U_REPORT));
  out.push_back(val(0x0010, U_REPORT, {}, true, false));  // boot keyboard input report（通知推送）
  out.push_back(desc(0x0011, T_REPORT_REF, {0x01, 0x01}, false));  // id=1, input
  out.push_back(desc(0x0012, T_CCC, {0x00, 0x00}, true));
  out.push_back(decl(0x0013, P_WRITE | P_WRITE_NO_RSP, U_REPORT));
  out.push_back(val(0x0014, U_REPORT, {}, false, true));  // output report
  out.push_back(desc(0x0015, T_REPORT_REF, {0x02, 0x02}, false));  // id=2, output
  // Battery
  out.push_back(svc(0x0016, U_BATTERY));
  out.push_back(decl(0x0017, P_READ | P_NOTIFY, U_BATTERY_LEVEL));
  out.push_back(val(0x0018, U_BATTERY_LEVEL, {s.battery_level}, true, false));
  out.push_back(desc(0x0019, T_CCC, {0x00, 0x00}, true));
  return out;
}

}  // namespace pb::ble
