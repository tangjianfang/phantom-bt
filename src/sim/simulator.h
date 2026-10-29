#pragma once
// Simulator：编排设备注册表、usbip 服务器与命令
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/btsnoop.h"
#include "sim/device_registry.h"
#include "usbip/server.h"

namespace pb::sim {

class Simulator {
 public:
  Simulator();
  ~Simulator();

  bool start(std::string* err, uint16_t want_port = 3240);  // 3240 起依次重试
  void stop();
  uint16_t port() const { return port_; }

  // 命令实现（文本协议，CLI/GUI/管道共享）；失败抛 std::runtime_error
  std::string cmd_load_hid_profile(const std::string& json_path);
  std::string cmd_load_bt_adapter();
  std::string cmd_ble_advertise(const std::string& json_path);  // → OK <addr>
  std::string cmd_ble_stop(const std::string& addr_hex);
  std::string cmd_ble_list();
  std::string cmd_ble_type(const std::string& addr_hex, const std::string& text);  // 逐字符两帧
  std::string cmd_list();
  std::string cmd_attach(const std::string& busid);
  std::string cmd_detach(const std::string& busid);
  std::string cmd_inject(const std::string& busid, const std::vector<uint8_t>& report);
  std::string cmd_status();

  // usbip.exe 检测：找到返回路径，否则空
  static std::string find_usbip_exe();

  sim::BtSnoopWriter& snoop() { return snoop_; }  // HCI 抓包（Wireshark 可开）

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;
  sim::BtSnoopWriter snoop_;
};

}  // namespace pb::sim
