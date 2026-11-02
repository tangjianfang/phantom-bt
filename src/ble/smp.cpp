#include "ble/smp.h"

#include <cstring>

#include "ble/aes.h"

namespace pb::ble {

std::array<uint8_t, 16> SmpResponder::s1(const uint8_t k[16], const uint8_t r1[16],
                                         const uint8_t r2[16]) {
  // Core 5.4 Vol3 H 2.2.4：s1(k,r1,r2)=e(k,r')，r' = r1 低64bit || r2 低64bit
  // （128 位值按 canonical MSB-left 显示序，低 64bit = 数组 octets 8-15，二者
  // 拼接而非 XOR）。Core 5.4 样例锚定（TK=0，r1=000F..88/r2=0102..00 →
  // 9a1fe1f0e8b0f49b5b4216ae796da062）。旧实现取 XOR 且高 8 字节清零——不符。
  uint8_t in[16] = {0};
  for (int i = 0; i < 8; ++i) {
    in[i] = r1[8 + i];
    in[8 + i] = r2[8 + i];
  }
  std::array<uint8_t, 16> out;
  crypto::aes128_encrypt_block(k, in, out.data());
  return out;
}

std::array<uint8_t, 16> SmpResponder::c1(const uint8_t tk[16], const uint8_t rand[16],
                                         const uint8_t preq[7], const uint8_t pres[7], uint8_t iat,
                                         const uint8_t ia[6], uint8_t rat, const uint8_t ra[6]) {
  // Core 5.4 Vol3 H 2.2.3（canonical MSB-left；AOSP stack_smp_test.cc 权威样例锚定，
  // 本地 .NET AES 复算穷举实证）：
  //   p1 = pres[0..6] || preq[0..6] || rat || iat
  //   p2 = 00×4 || ia[0..5] || ra[0..5]（地址按显示序原样，填充在高端）
  //   c1 = e(tk, e(tk, rand ⊕ p1) ⊕ p2)
  // 样例：TK=0, rand=5783..E0, preq=0707..0101, pres=0500..0302, iat=1/rat=0,
  // ia=A1..A6, ra=B1..B6 → 1e1e3fef878988ead2a74dc5bef13b86（中间值
  // rand⊕p1=5283dd2156ae6d096498274ec7712ee1 亦与样例注释一致）。
  // 旧实现 p1 为 preq||pres 且 iat/rat 互换、p2 把 ia/ra 反序且填充在尾部——
  // 16 种旧式排布组合均不可达样例值。
  uint8_t r[16];
  std::memcpy(r, rand, 16);
  for (int i = 0; i < 7; ++i) r[i] ^= pres[i];
  for (int i = 0; i < 7; ++i) r[7 + i] ^= preq[i];
  r[14] ^= rat;
  r[15] ^= iat;
  uint8_t p1[16];
  crypto::aes128_encrypt_block(tk, r, p1);
  uint8_t p2in[16] = {0};
  for (int i = 0; i < 6; ++i) {
    p2in[4 + i] = ia[i];
    p2in[10 + i] = ra[i];
  }
  uint8_t x[16];
  for (int i = 0; i < 16; ++i) x[i] = p1[i] ^ p2in[i];
  std::array<uint8_t, 16> out;
  crypto::aes128_encrypt_block(tk, x, out.data());
  return out;
}

void SmpResponder::pack_c1_address(const uint8_t lsb[6], uint8_t display[6]) {
  for (int i = 0; i < 6; ++i) display[i] = lsb[5 - i];
}

void SmpResponder::set_connection_addresses(const uint8_t central_lsb[6], uint8_t central_type,
                                           const uint8_t peripheral_lsb[6], uint8_t peripheral_type) {
  pack_c1_address(central_lsb, ia_);
  pack_c1_address(peripheral_lsb, ra_);
  iat_ = central_type ? 1 : 0;
  rat_ = peripheral_type ? 1 : 0;
  addrs_set_ = addresses_nonzero();
}

bool SmpResponder::addresses_nonzero() const {
  for (int i = 0; i < 6; ++i)
    if (ia_[i] | ra_[i]) return true;
  return false;
}

std::array<uint8_t, 7> SmpResponder::pairing_response() const {
  // MaxEncryptionKeySize = 16，与实际 16 字节 STK 一致（规范下限 7 会让主机截短密钥）
  return {0x02, 0x03, 0x00, preq_[3], 0x10, 0x02, 0x02};
}

std::optional<std::vector<uint8_t>> SmpResponder::handle(const uint8_t* pdu, size_t n) {
  if (n < 1) return std::nullopt;
  uint8_t code = pdu[0];

  switch (code) {
    case 0x01: {  // Pairing Request（主机发起）
      if (n < 7) return std::vector<uint8_t>{0x05, 0x0A};  // Pairing Failed: Invalid Parameters（2 字节）
      std::memcpy(preq_.data(), pdu, 7);
      confirm_checked_ = false;
      paired_ = false;
      auto pres = pairing_response();
      return std::vector<uint8_t>(pres.begin(), pres.end());
    }
    case 0x03: {  // Pairing Confirm（主机 c1(TK=0, Mrand, 真实地址)）
      if (n < 17) return std::nullopt;
      std::memcpy(mconfirm_.data(), pdu + 1, 16);
      if (!addrs_set_ || !addresses_nonzero()) {
        // 没有连接双方的真实地址就不能算 confirm；禁止退回全 0 地址。
        return std::vector<uint8_t>{0x05, 0x04};  // Confirm Value Failed
      }
      for (int i = 0; i < 16; ++i) srand_[i] = (uint8_t)(0x10 + i);
      uint8_t tk[16] = {0};
      auto pres = pairing_response();
      auto confirm = c1(tk, srand_.data(), preq_.data(), pres.data(), iat_, ia_, rat_, ra_);
      std::vector<uint8_t> rsp{0x03};
      rsp.insert(rsp.end(), confirm.begin(), confirm.end());
      return rsp;
    }
    case 0x04: {  // Pairing Random（主机 Mrand）—— 先复核主机 confirm
      if (n < 17) return std::nullopt;
      std::memcpy(mrand_, pdu + 1, 16);
      if (!addrs_set_ || !addresses_nonzero()) {
        paired_ = false;
        return std::vector<uint8_t>{0x05, 0x04};
      }
      uint8_t tk[16] = {0};
      auto pres = pairing_response();
      auto expect = c1(tk, mrand_, preq_.data(), pres.data(), iat_, ia_, rat_, ra_);
      if (std::memcmp(expect.data(), mconfirm_.data(), 16) != 0) {
        paired_ = false;
        confirm_checked_ = false;
        return std::vector<uint8_t>{0x05, 0x04};  // Confirm Value Failed（2 字节）
      }
      confirm_checked_ = true;
      stk_ = s1(tk, srand_.data(), mrand_);
      paired_ = true;
      std::vector<uint8_t> rsp{0x04};
      rsp.insert(rsp.end(), srand_.begin(), srand_.end());
      return rsp;
    }
    default:
      return std::nullopt;  // 其余（密钥分发等）静默
  }
}

}  // namespace pb::ble
