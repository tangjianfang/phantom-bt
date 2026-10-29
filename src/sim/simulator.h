#pragma once
// Simulator：编排设备注册表、usbip 服务器与命令
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
  std::string cmd_list();
  std::string cmd_attach(const std::string& busid);
  std::string cmd_detach(const std::string& busid);
  std::string cmd_inject(const std::string& busid, const std::vector<uint8_t>& report);
  std::string cmd_status();

  // usbip.exe 检测：找到返回路径，否则空
  static std::string find_usbip_exe();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;
};

}  // namespace pb::sim
