#pragma once
// MiniController：自研精简 HCI 控制器引擎（进程内，无真实射频）
// M1：初始化命令子集；M2/M3 扩展 LE 空口与链路（le_air/link）
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hci/packets.h"

namespace pb::hci {

class VirtualAir;  // M2 前置声明

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
  void attach_air(VirtualAir* air) { air_ = air; }
  void tick(uint64_t now_ms);             // 定时驱动（空口广播/延迟事件）
  void tick_for_tests(uint64_t now_ms) { tick(now_ms); }

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
  VirtualAir* air_ = nullptr;
  std::string local_name_stored_;
  std::array<uint8_t, 8> eir_{};
};

// 命令位图中的位序：Supported Commands 表 pos = (ogf-1)*64 + (ocf & 0x3F)
size_t supported_commands_bit(uint16_t op);
// 本引擎支持的命令列表（Read_Local_Supported_Commands 与一致性检查用）
const std::vector<uint16_t>& supported_command_list();

}  // namespace pb::hci
