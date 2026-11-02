#pragma once
// SMP Legacy Just Works 应答端（外设角色，无输入无输出）
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace pb::ble {

class SmpResponder {
 public:
  // 处理一条 SMP PDU（CID 0x0006）；返回应答或 nullopt
  std::optional<std::vector<uint8_t>> handle(const uint8_t* pdu, size_t n);
  bool paired() const { return paired_; }
  const uint8_t* stk() const { return stk_.data(); }
  // HCI 地址是 LSB first。c1 使用显示序（MSB 在前）以及连接双方的地址类型
  //（0=public，1=random）。central 是 SMP initiator（本控制器作 central）。
  void set_connection_addresses(const uint8_t central_lsb[6], uint8_t central_type,
                                const uint8_t peripheral_lsb[6], uint8_t peripheral_type);
  static void pack_c1_address(const uint8_t lsb[6], uint8_t display[6]);
  bool has_addresses() const { return addrs_set_; }
  uint8_t initiating_addr_type() const { return iat_; }
  uint8_t responding_addr_type() const { return rat_; }
  const uint8_t* initiating_addr() const { return ia_; }    // 6 bytes, display order
  const uint8_t* responding_addr() const { return ra_; }    // 6 bytes, display order
  // STK = s1(TK=0, Srand, Mrand)：s1(k,r1,r2) = e(k, r1低64bit || r2低64bit)（拼接）
  static std::array<uint8_t, 16> s1(const uint8_t k[16], const uint8_t r1[16],
                                    const uint8_t r2[16]);
  // c1(TK, rand, pres, preq, iat, ia, rat, ra) —— Core Vol3 H 2.2.3
  static std::array<uint8_t, 16> c1(const uint8_t tk[16], const uint8_t rand[16],
                                    const uint8_t preq[7], const uint8_t pres[7], uint8_t iat,
                                    const uint8_t ia[6], uint8_t rat, const uint8_t ra[6]);

 private:
  bool paired_ = false;
  std::array<uint8_t, 16> stk_{};
  std::array<uint8_t, 7> preq_{};  // 主机 Pairing Request（c1 用）
  std::array<uint8_t, 16> srand_{};
  std::array<uint8_t, 16> mconfirm_{};  // 主机 Pairing Confirm（Random 阶段校验）
  uint8_t mrand_[16]{};
  bool addrs_set_ = false;
  uint8_t iat_ = 0;
  uint8_t rat_ = 0;
  uint8_t ia_[6]{};
  uint8_t ra_[6]{};
  bool confirm_checked_ = false;

  std::array<uint8_t, 7> pairing_response() const;
  bool addresses_nonzero() const;
};

}  // namespace pb::ble
