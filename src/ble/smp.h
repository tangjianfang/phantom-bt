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
  // STK = s1(TK=0, Srand, Mrand)：s1(k,r1,r2) = e(k, r1'⊕r2')，r' 取低 64bit
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
  uint8_t mrand_[16];
};

}  // namespace pb::ble
