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
  std::string cmd_ble_clear();  // stop every BLE advertiser
  std::string cmd_ble_list();
  std::string cmd_ble_type(const std::string& addr_hex, const std::string& text);  // 逐字符两帧
  std::string cmd_list();
  std::string cmd_unload(const std::string& busid);  // detach + drop the device model
  std::string cmd_unload_all();
  // Physical radio control (SetupAPI): "list" | "disable" | "enable".
  // State changes require elevation; throws with the reason on failure.
  std::string cmd_sys_bt(const std::string& op);
  std::string cmd_attach(const std::string& busid);
  std::string cmd_detach(const std::string& busid);
  std::string cmd_inject(const std::string& busid, const std::vector<uint8_t>& report);
  std::string cmd_status();
  // HCI 抓包控制：`off` 关闭写者并从已加载适配器摘钩（跨后续 load-bt 保持关闭）；
  // 其余参数视为路径重定向抓包（打不开抛 std::runtime_error）。
  std::string cmd_snoop(const std::string& path_or_off);

  // usbip.exe 检测：找到返回路径，否则空
  static std::string find_usbip_exe();

  // —— 缺陷修复批引入的可测静态（gui-port / detach-by-device）——
  // 内嵌服务器端口约定：环境变量 PB_PORT（十进制 1-65535）；未设置/非法回落
  // 默认 3425（usbip 客户端默认端口 3240 被本机其它项目常驻占用）。
  static uint16_t server_port_from_env();
  // usbip attach 命令行（两种语法依次尝试）：服务器不在 usbip 默认端口 3240 时
  // 第一条命令必须带全局选项 -t（usbip-win2 的 -t 须在子命令之前）。
  static std::vector<std::string> attach_commands(const std::string& exe, uint16_t port,
                                                  const std::string& busid);
  // "usbip port" 输出解析：每个 "Port NN:" 块与其后的 "-> usbip://host:port/<busid>"
  // 行组成 busid→NN 映射（busid 可能属于任何服务器端口，映射只认导入块本身）。
  static std::map<std::string, int> parse_imported_ports(const std::string& port_output);
  // 按设备卸载计划：busid 已导入 → "\"<exe>\" detach -p <NN>"；未导入 → 空串
  // （无事可做，绝不 fallback 到 detach --all）。
  static std::string detach_command_for(const std::string& exe, const std::string& port_output,
                                        const std::string& busid);

  sim::BtSnoopWriter& snoop() { return snoop_; }  // HCI 抓包（Wireshark 可开）

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;
  sim::BtSnoopWriter snoop_;
  bool snoop_off_ = false;  // snoop off 后 load-bt 不再自动开默认抓包
};

}  // namespace pb::sim
