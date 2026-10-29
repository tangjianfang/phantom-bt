#include "ble/smp.h"

#include <cstring>

#include "ble/aes.h"

namespace pb::ble {

std::array<uint8_t, 16> SmpResponder::s1(const uint8_t k[16], const uint8_t r1[16],
                                         const uint8_t r2[16]) {
  uint8_t in[16] = {0};
  for (int i = 0; i < 8; ++i) in[i] = r1[8 + i] ^ r2[8 + i];  // 低 64bit = 各 rand 低 8 字节
  std::array<uint8_t, 16> out;
  crypto::aes128_encrypt_block(k, in, out.data());
  return out;
}

std::array<uint8_t, 16> SmpResponder::c1(const uint8_t tk[16], const uint8_t rand[16],
                                         const uint8_t preq[7], const uint8_t pres[7], uint8_t iat,
                                         const uint8_t ia[6], uint8_t rat, const uint8_t ra[6]) {
  uint8_t r[16];
  std::memcpy(r, rand, 16);
  for (int i = 0; i < 7; ++i) r[i] ^= preq[i];
  for (int i = 0; i < 7; ++i) r[7 + i] ^= pres[i];
  r[14] ^= iat;
  r[15] ^= rat;
  uint8_t p1[16];
  crypto::aes128_encrypt_block(tk, r, p1);
  // p1 后与地址组合：ia、ra 各 6B（低字节序）
  uint8_t p2in[16] = {0};
  for (int i = 0; i < 6; ++i) {
    p2in[i] = ia[5 - i];
    p2in[6 + i] = ra[5 - i];
  }
  uint8_t x[16];
  for (int i = 0; i < 16; ++i) x[i] = p1[i] ^ p2in[i];
  std::array<uint8_t, 16> out;
  crypto::aes128_encrypt_block(tk, x, out.data());
  return out;
}

std::optional<std::vector<uint8_t>> SmpResponder::handle(const uint8_t* pdu, size_t n) {
  if (n < 1) return std::nullopt;
  uint8_t code = pdu[0];

  switch (code) {
    case 0x01: {  // Pairing Request（主机发起）
      if (n < 7) return std::vector<uint8_t>{0x05, 0x01, 0x00, 0x0A};  // Pairing Failed: Invalid params
      std::memcpy(preq_.data(), pdu, 7);
      // 应答：IO=NoInputNoOutput(3)、OOB=0、authreq=保持主机值、key dist 最小
      return std::vector<uint8_t>{0x02, 0x03, 0x00, pdu[3], 0x07, 0x02, 0x02};
    }
    case 0x03: {  // Pairing Confirm（主机 c1(TK=0, Mrand,...)）
      if (n < 17) return std::nullopt;
      // 我们生成 Srand 并回 Confirm（TK=0 Just Works）
      for (int i = 0; i < 16; ++i) srand_[i] = (uint8_t)(0x10 + i);
      uint8_t tk[16] = {0};
      uint8_t pres[7] = {0x02, 0x03, 0x00, preq_[3], 0x07, 0x02, 0x02};
      // 地址对 Just Works 全 0（进程内伪造链路，无真实射频）
      uint8_t zero6[6] = {0};
      auto confirm = c1(tk, srand_.data(), preq_.data(), pres, 0, zero6, 0, zero6);
      std::vector<uint8_t> rsp{0x03};
      rsp.insert(rsp.end(), confirm.begin(), confirm.end());
      return rsp;
    }
    case 0x04: {  // Pairing Random（主机 Mrand）
      if (n < 17) return std::nullopt;
      std::memcpy(mrand_, pdu + 1, 16);
      uint8_t tk[16] = {0};
      stk_ = s1(tk, srand_.data(), mrand_);
      paired_ = true;
      std::vector<uint8_t> rsp{0x05};
      rsp.insert(rsp.end(), srand_.begin(), srand_.end());
      return rsp;
    }
    default:
      return std::nullopt;  // 其余（密钥分发等）静默
  }
}

}  // namespace pb::ble
