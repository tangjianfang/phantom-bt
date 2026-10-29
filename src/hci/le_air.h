#pragma once
// VirtualAir：虚拟 BLE 空口（内存队列），承载广播源与连接查找
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pb::hci {

struct AdvPayload {
  std::array<uint8_t, 31> adv{};
  uint8_t adv_len = 0;
  std::array<uint8_t, 31> scan_rsp{};
  uint8_t scan_rsp_len = 0;
};

class VirtualAdvertiser {
 public:
  // addr：静态随机地址（高字节 0xC0..），值序 addr[5]<<40|...|addr[0]（与 HCI 报文 LSB-first 一致）
  VirtualAdvertiser(uint64_t addr, std::string name, bool connectable = true);

  void set_adv_data(const std::vector<uint8_t>& raw31);
  void set_adv_data_name(std::string_view name, bool add_flags = true);
  void set_adv_data_manufacturer(uint16_t company, const std::vector<uint8_t>& payload);
  void set_scan_response(std::string_view name);
  void set_connectable(bool c) { connectable_ = c; }
  uint64_t address() const { return addr_; }
  bool connectable() const { return connectable_; }
  const AdvPayload& payload() const { return pl_; }
  const std::string& name() const { return name_; }

 private:
  void append_ad(std::array<uint8_t, 31>& buf, uint8_t& len, uint8_t type,
                 const std::vector<uint8_t>& body);
  uint64_t addr_;
  bool connectable_;
  std::string name_;
  AdvPayload pl_;
};

class VirtualAir {
 public:
  void add(std::shared_ptr<VirtualAdvertiser> adv);
  void remove(uint64_t addr);
  void set_host_scanning(bool enabled, bool active);
  // tick：主机扫描中且距该源上次报告 ≥ interval（默认 128ms）→ 生成 ADV_IND（+SCAN_RSP 当 active）
  void tick(uint64_t now_ms, std::function<void(std::vector<uint8_t> le_meta_event)> emit);
  std::shared_ptr<VirtualAdvertiser> find_connectable(uint64_t addr);
  std::shared_ptr<VirtualAdvertiser> find_any(uint64_t addr);
  std::vector<std::shared_ptr<VirtualAdvertiser>> devices() const;
  bool scanning() const { return scanning_; }
  bool active_scan() const { return active_; }

 private:
  std::vector<std::shared_ptr<VirtualAdvertiser>> advs_;
  bool scanning_ = false;
  bool active_ = false;
  std::vector<uint64_t> last_tick_;
  static constexpr uint64_t kIntervalMs = 128;
};

// LE Advertising Report 事件（EVT_LE_META/0x02）组包
std::vector<uint8_t> build_adv_report_event(uint8_t evt_type, uint8_t addr_type, uint64_t addr,
                                            const uint8_t* data, uint8_t data_len, int8_t rssi = -64);

}  // namespace pb::hci
