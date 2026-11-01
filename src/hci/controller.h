#pragma once
// MiniController：自研精简 HCI 控制器引擎（进程内，无真实射频）
// M1：初始化命令子集；M2：LE 空口（扫描/广播/连接）；M3：链路数据面
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hci/le_air.h"
#include "hci/link.h"
#include "hci/packets.h"

namespace pb::hci {

struct ControllerConfig {
  std::array<uint8_t, 6> bd_addr = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};  // LSB first
  std::string local_name = "Phantom-BT Radio";
  uint16_t acl_mtu = 1021;
  uint16_t num_acl_packets = 32;
  uint16_t le_acl_mtu = 251;
  uint8_t le_num_packets = 8;
  uint8_t hci_version = 0x0B;  // Bluetooth 5.2
  uint16_t hci_revision = 0x0000;
  uint8_t lmp_version = 0x0B;
  uint16_t lmp_subversion = 0xFFFF;
  uint16_t manufacturer = 0xFFFF;
};

class MiniController {
 public:
  explicit MiniController(const ControllerConfig& cfg);
  ~MiniController();

  // —— 主机侧（USB 适配器模型调用）——
  void handle_command(const uint8_t* p, size_t n);   // 应答事件入队
  void handle_acl_from_host(const AclFrame&);        // M3 起交给链路层；M1 记日志
  bool pop_event(std::vector<uint8_t>& out);         // INT IN 拉取
  bool pop_acl_to_host(AclFrame& out);               // BULK IN 拉取
  void push_event(const std::vector<uint8_t>& evt);  // 空口/链路注入

  // —— 观测/抓包（可选：由 Simulator 挂接 btsnoop 记录器）——
  std::function<void(const std::vector<uint8_t>&)> snoop_command;  // 主机→控制器 HCI 命令
  std::function<void(const std::vector<uint8_t>&)> snoop_event;    // 控制器→主机 事件
  std::function<void(const AclFrame&)> snoop_acl_in;               // 主机→控制器 ACL
  std::function<void(const AclFrame&)> snoop_acl_out;              // 控制器→主机 ACL

  // —— 观测（测试/诊断）——
  std::vector<std::string> op_log() const;
  int unknown_opcodes_logged() const { return unknown_count_; }
  uint32_t event_count() const { return event_count_; }
  bool radio_ready() const { return radio_ready_; }
  const ControllerConfig& config() const { return cfg_; }
  uint8_t scan_enable() const { return scan_enable_; }
  bool le_scan_enabled() const { return le_scan_enabled_; }
  bool le_scan_active() const { return le_scan_active_; }

  // —— M2 空口接入 ——
  VirtualAir& air() { return air_; }      // 控制器自有空口（广播源注册于此）
  void tick(uint64_t now_ms);             // 定时驱动（空口广播/连接延迟事件）
  void tick_for_tests(uint64_t now_ms) { tick(now_ms); }
  bool has_connection() const { return conn_handle_ != 0; }
  uint16_t connection_handle() const { return conn_handle_; }
  uint64_t connection_peer() const { return conn_peer_; }
  void simulate_disconnect(uint8_t reason);  // 断开当前连接（设备侧/测试触发）

  // —— M3 链路数据面 ——
  // 主机方向完整 L2CAP PDU 回调（ATT CID 0x0004 / SM 0x0006 由上层分发）
  std::function<void(uint16_t handle, const std::vector<uint8_t>& pdu)> on_l2cap_from_host;
  // 设备侧发送完整 L2CAP PDU → ACL 切片入队（BULK IN 侧取走）
  void send_l2cap(uint16_t handle, const std::vector<uint8_t>& pdu);
  // 事件/ACL 出队方向有新数据（tick 广播报告、连接完成、设备侧主动 L2CAP 等
  // 非主机命令触发的输出）。适配器模型挂接以完成挂起的 INT IN / BULK IN URB。
  std::function<void()> on_ready_to_host;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ControllerConfig cfg_;
  mutable std::mutex mx_;
  std::deque<std::vector<uint8_t>> event_q_;
  std::deque<AclFrame> acl_to_host_q_;
  std::vector<std::string> op_log_;
  int unknown_count_ = 0;
  uint32_t event_count_ = 0;
  bool radio_ready_ = false;
  bool seen_reset_ = false;
  bool seen_bd_addr_ = false;
  uint8_t scan_enable_ = 0;
  bool le_scan_enabled_ = false;
  bool le_scan_active_ = false;
  VirtualAir air_;            // 自有虚拟空口
  LinkTable links_;           // M3：连接句柄表
  AclReassembler reasm_;      // 主机→设备 ACL 重组
  // ACL 流控：主机→控制器方向的在途包（句柄 → 已收未完成包数）；
  // 每个 HCI ACL 数据包占用一个控制器缓冲，发送完成后以
  // Number_Of_Completed_Packets 事件归还主机 credit（Vol4 PartE 7.7.19）
  std::map<uint16_t, uint16_t> acl_inflight_;
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> delayed_events_;  // (tick 倒计时, 事件)
  std::string local_name_stored_;
  std::array<uint8_t, 8> eir_{};
  // 连接创建延迟状态
  bool conn_pending_ = false;
  uint64_t conn_pending_peer_ = 0;
  uint8_t conn_pending_type_ = 0;
  uint64_t conn_fire_at_ = 0;
  uint16_t conn_handle_ = 0;
  uint16_t next_handle_ = 0x0001;
  uint64_t conn_peer_ = 0;
};

// 命令在 Supported Commands 位图（Vol 4 Part E §6.27 规范表格）中的位号；
// 未收录命令返回 ≥512（超出 64 字节位图，表示不可表示）
size_t supported_commands_bit(uint16_t op);
// 本引擎支持的命令列表（Read_Local_Supported_Commands 与一致性检查用）
const std::vector<uint16_t>& supported_command_list();

}  // namespace pb::hci
