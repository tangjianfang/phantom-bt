#pragma once
// USB 标准控制请求分发 + 设备模型接口
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

#include "usb/descriptors.h"
#include "usbip/protocol.h"

namespace pb::usb {

// 标准请求号
constexpr uint8_t REQ_GET_STATUS = 0, REQ_CLEAR_FEATURE = 1, REQ_SET_FEATURE = 3, REQ_SET_ADDRESS = 5,
                  REQ_GET_DESCRIPTOR = 6, REQ_SET_DESCRIPTOR = 7, REQ_GET_CONFIGURATION = 8,
                  REQ_SET_CONFIGURATION = 9, REQ_GET_INTERFACE = 10, REQ_SET_INTERFACE = 11;
// 描述符类型
constexpr uint8_t DT_DEVICE = 1, DT_CONFIG = 2, DT_STRING = 3, DT_INTERFACE = 4, DT_ENDPOINT = 5,
                  DT_DEVICE_QUALIFIER = 6, DT_IAD = 0x0B, DT_BOS = 0x0F, DT_HID_REPORT = 0x22;

// EP 状态码（ret_submit status 用）
constexpr int32_t EP_STATUS_OK = 0;
constexpr int32_t EP_STATUS_STALL = -32;  // EPIPE

struct ControlSetup {
  uint8_t bmRequestType, bRequest;
  uint16_t wValue, wIndex, wLength;
};

inline ControlSetup parse_setup(const uint8_t s[8]) {
  return ControlSetup{s[0], s[1],
                      (uint16_t)(s[2] | (s[3] << 8)),
                      (uint16_t)(s[4] | (s[5] << 8)),
                      (uint16_t)(s[6] | (s[7] << 8))};
}

// 模型抛出此异常表示 STALL
struct ControlStall : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// 描述符仓库：模型填充，descriptor_request 消费
struct DescriptorStore {
  std::vector<uint8_t> device;        // 18B
  std::vector<uint8_t> config;        // 完整配置描述符
  std::vector<uint8_t> report_descriptor;  // HID 报告描述符（可选）
  std::map<uint8_t, std::vector<uint8_t>> strings;  // index → 描述符字节（0 = LANGID）
};

// 处理标准（bmRequestType 类型段=0）GET_DESCRIPTOR 与基础标准请求。
// 返回 true 表示已处理并填充 out（应答数据，调用方按 wLength 截断）；false = 未识别交由模型。
bool descriptor_request(const ControlSetup& s, const DescriptorStore& store, std::vector<uint8_t>& out);

// 设备模型接口：HID / 蓝牙适配器实现
struct IUsbModelSink {
  virtual ~IUsbModelSink() = default;
  // 异步完成一个挂起的 IN 传输（bulk/interrupt IN）
  virtual void complete_in(uint8_t ep_addr, int32_t status, std::vector<uint8_t> data) = 0;
  virtual void stall_ep(uint8_t ep_addr) = 0;
};

class IUsbDeviceModel {
 public:
  virtual ~IUsbDeviceModel() = default;
  virtual const usbip::ExportedDevice& exported() const = 0;
  virtual void attach_sink(IUsbModelSink* sink) = 0;
  // EP0：返回 IN 方向应答数据；抛 ControlStall 表示 STALL
  virtual std::vector<uint8_t> on_control(const ControlSetup& s, const std::vector<uint8_t>& data) = 0;
  // 数据 OUT 传输完成
  virtual void on_out(uint8_t ep_addr, std::vector<uint8_t> data) = 0;
  // IN 传输到达（轮询）：有数据立即 complete_in；否则记住关注
  virtual void on_in(uint8_t ep_addr) = 0;
  virtual void on_stall_clear(uint8_t ep_addr) {}
  virtual void on_detach() = 0;
};

}  // namespace pb::usb
