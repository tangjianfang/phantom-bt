#pragma once
// HOGP 行为机：CCC 订阅、协议模式、输入报告通知、输出报告接收
#include <cstdint>
#include <functional>
#include <vector>

#include "ble/gatt_db.h"

namespace pb::ble {

class Hogp {
 public:
  explicit Hogp(GattDb& db);
  // ATT 写完成后的钩子（write_value 已写入后调用）：控制点/协议模式/输出报告
  void on_write(uint16_t handle, const std::vector<uint8_t>& v);
  // 推送键盘输入报告（8B：mod, reserved, k1..k6）；CCC 未订阅返回 false
  bool send_input_report(const std::vector<uint8_t>& keys8);
  // 通知出口：上层包 L2CAP(CID 0x0004) → controller.send_l2cap
  std::function<void(const std::vector<uint8_t>& att_pdu)> notify_post;
  uint8_t protocol_mode() const { return protocol_mode_; }
  bool suspended() const { return suspended_; }
  int dropped_reports() const { return dropped_; }
  std::vector<std::vector<uint8_t>> output_reports;  // 主机写入的输出报告

 private:
  bool ccc_subscribed(uint16_t ccc_handle) const;
  GattDb& db_;
  uint8_t protocol_mode_ = 0x01;  // report
  bool suspended_ = false;
  int dropped_ = 0;
};

}  // namespace pb::ble
