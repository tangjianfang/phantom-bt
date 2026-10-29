#include "usb/control.h"

#include "core/bytes.h"

namespace pb::usb {

namespace {
// bmRequestType: bit7 方向(0=OUT,1=IN)，bit6-5 类型(00=标准,01=类,10=厂商)，bit4-0 接收者
constexpr uint8_t kTypeMask = 0x60;
constexpr uint8_t kTypeStandard = 0x00;
}  // namespace

bool descriptor_request(const ControlSetup& s, const DescriptorStore& store, std::vector<uint8_t>& out) {
  if ((s.bmRequestType & kTypeMask) != kTypeStandard) return false;

  switch (s.bRequest) {
    case REQ_GET_DESCRIPTOR: {
      uint8_t dtype = s.wValue >> 8;
      uint8_t index = s.wValue & 0xFF;
      const std::vector<uint8_t>* src = nullptr;
      switch (dtype) {
        case DT_DEVICE: src = &store.device; break;
        case DT_CONFIG: src = &store.config; break;
        case DT_STRING: {
          auto it = store.strings.find(index);
          if (it == store.strings.end()) return false;  // 交由模型（或 STALL）
          src = &it->second;
          break;
        }
        case DT_DEVICE_QUALIFIER: {
          // FS 设备非合规限定符：返回 STALL 语义（返回 false 让模型决定）
          return false;
        }
        default: return false;  // 0x22 报告描述符等类描述符归模型
      }
      if (!src || src->empty()) return false;
      out.assign(src->begin(), src->end());
      if (out.size() > s.wLength) out.resize(s.wLength);  // 截断到 wLength
      return true;
    }
    case REQ_SET_ADDRESS:
    case REQ_SET_CONFIGURATION:
      out.clear();
      return true;
    case REQ_GET_CONFIGURATION:
      out = {1};
      if (out.size() > s.wLength) out.resize(s.wLength);
      return true;
    case REQ_GET_STATUS: {
      // 标准 GET_STATUS：设备状态 2 字节（自供电位按描述符属性，远程唤醒位）
      out = {0x00, 0x00};
      if (out.size() > s.wLength) out.resize(s.wLength);
      return true;
    }
    case REQ_SET_INTERFACE:
      out.clear();
      return true;
    default:
      return false;
  }
}

}  // namespace pb::usb
