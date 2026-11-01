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
      std::memcpy(mconfirm_.data(), pdu + 1, 16);  // Random 阶段校验用
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
      // Vol 3 Part H 3.3：对 Pairing Random 应回 0x04 + Srand
      //（回 0x05=Pairing Failed 会让合规主机立即中止配对）
      std::vector<uint8_t> rsp{0x04};
      rsp.insert(rsp.end(), srand_.begin(), srand_.end());
      return rsp;
    }
    default:
      return std::nullopt;  // 其余（密钥分发等）静默
  }
}

}  // namespace pb::ble
