#include "hci/le_air.h"

#include <algorithm>
#include <cstring>

#include "hci/packets.h"

namespace pb::hci {

namespace {
constexpr uint64_t kNever = ~0ull;  // 从未触发过 tick 的哨兵（首次 tick 必发报）
}

VirtualAdvertiser::VirtualAdvertiser(uint64_t addr, std::string name, bool connectable)
    : addr_(addr), connectable_(connectable), name_(std::move(name)) {}

void VirtualAdvertiser::append_ad(std::array<uint8_t, 31>& buf, uint8_t& len, uint8_t type,
                                  const std::vector<uint8_t>& body) {
  if (len + 2 + body.size() > 31) return;  // 放不下则丢弃
  buf[len++] = (uint8_t)(body.size() + 1);
  buf[len++] = type;
  std::memcpy(buf.data() + len, body.data(), body.size());
  len = (uint8_t)(len + body.size());
}

void VirtualAdvertiser::set_adv_data(const std::vector<uint8_t>& raw31) {
  size_t n = std::min<size_t>(raw31.size(), 31);
  std::memcpy(pl_.adv.data(), raw31.data(), n);
  pl_.adv_len = (uint8_t)n;
}

void VirtualAdvertiser::set_adv_data_name(std::string_view name, bool add_flags) {
  pl_.adv = {};
  pl_.adv_len = 0;
  if (add_flags) append_ad(pl_.adv, pl_.adv_len, 0x01, {0x06});  // LE General Discoverable + BR/EDR Not Supported
  std::vector<uint8_t> nm(name.begin(), name.end());
  if (nm.size() > 29 - 2) nm.resize(29 - 2);  // 完整名 AD 上限
  append_ad(pl_.adv, pl_.adv_len, 0x09, nm);  // Complete Local Name
}

void VirtualAdvertiser::set_adv_data_manufacturer(uint16_t company, const std::vector<uint8_t>& payload) {
  pl_.adv = {};
  pl_.adv_len = 0;
  append_ad(pl_.adv, pl_.adv_len, 0x01, {0x06});
  std::vector<uint8_t> ms;
  ms.push_back(company & 0xFF);
  ms.push_back(company >> 8);
  ms.insert(ms.end(), payload.begin(), payload.end());
  if (ms.size() > 29 - 2) ms.resize(29 - 2);
  append_ad(pl_.adv, pl_.adv_len, 0xFF, ms);  // Manufacturer Specific Data
}

void VirtualAdvertiser::set_scan_response(std::string_view name) {
  pl_.scan_rsp = {};
  pl_.scan_rsp_len = 0;
  std::vector<uint8_t> nm(name.begin(), name.end());
  if (nm.size() > 29 - 2) nm.resize(29 - 2);
  append_ad(pl_.scan_rsp, pl_.scan_rsp_len, 0x09, nm);
}

void VirtualAir::add(std::shared_ptr<VirtualAdvertiser> adv) {
  std::lock_guard<std::mutex> lk(mx_);
  advs_.push_back(std::move(adv));
}

void VirtualAir::remove(uint64_t addr) {
  std::lock_guard<std::mutex> lk(mx_);
  advs_.erase(std::remove_if(advs_.begin(), advs_.end(),
                             [addr](auto& a) { return a->address() == addr; }),
              advs_.end());
}

void VirtualAir::set_host_scanning(bool enabled, bool active) {
  std::lock_guard<std::mutex> lk(mx_);
  scanning_ = enabled;
  active_ = active;
}

std::shared_ptr<VirtualAdvertiser> VirtualAir::find_connectable(uint64_t addr) {
  std::lock_guard<std::mutex> lk(mx_);
  for (auto& a : advs_)
    if (a->address() == addr && a->connectable()) return a;
  return nullptr;
}

std::shared_ptr<VirtualAdvertiser> VirtualAir::find_any(uint64_t addr) {
  std::lock_guard<std::mutex> lk(mx_);
  for (auto& a : advs_)
    if (a->address() == addr) return a;
  return nullptr;
}

std::vector<std::shared_ptr<VirtualAdvertiser>> VirtualAir::devices() const {
  std::lock_guard<std::mutex> lk(mx_);
  return advs_;
}

std::vector<uint8_t> build_adv_report_event(uint8_t evt_type, uint8_t addr_type, uint64_t addr,
                                            const uint8_t* data, uint8_t data_len, int8_t rssi) {
  // params: num(1) evt_type(1) addr_type(1) addr(6) len(1) data[31] rssi(1)（build_le_meta 前置 subevent）
  std::vector<uint8_t> p;
  p.push_back(0x01);  // num reports
  p.push_back(evt_type);
  p.push_back(addr_type);
  for (int i = 0; i < 6; ++i) p.push_back((addr >> (8 * i)) & 0xFF);  // LSB first
  p.push_back(data_len);
  p.insert(p.end(), data, data + data_len);
  p.push_back((uint8_t)rssi);
  return build_le_meta(LE_EVT_ADVERTISING_REPORT, p);
}

void VirtualAir::tick(uint64_t now_ms, std::function<void(std::vector<uint8_t>)> emit) {
  std::lock_guard<std::mutex> lk(mx_);
  if (!scanning_) {
    last_tick_.assign(advs_.size(), kNever);
    return;
  }
  if (last_tick_.size() != advs_.size()) last_tick_.assign(advs_.size(), kNever);
  for (size_t i = 0; i < advs_.size(); ++i) {
    uint64_t last = last_tick_[i];
    if (last != kNever && now_ms - last < kIntervalMs) continue;
    last_tick_[i] = now_ms;
    auto& a = advs_[i];
    uint8_t evt = a->connectable() ? 0x00 : 0x02;  // ADV_IND / ADV_NONCONN_IND
    emit(build_adv_report_event(evt, 1, a->address(), a->payload().adv.data(),
                                a->payload().adv_len));
    if (active_ && a->connectable() && a->payload().scan_rsp_len > 0) {
      emit(build_adv_report_event(0x04, 1, a->address(), a->payload().scan_rsp.data(),
                                  a->payload().scan_rsp_len));  // SCAN_RSP
    }
  }
}

}  // namespace pb::hci
