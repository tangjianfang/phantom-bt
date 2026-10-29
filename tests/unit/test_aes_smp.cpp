#include "test/test_framework.h"
#include "ble/aes.h"
#include "ble/smp.h"

#include "core/bytes.h"
#include <cstdio>

using namespace pb;

// —— 公开测试向量（外部正确性锚点，防自证）——
PB_TEST(aes_fips197_c1_vector) {
  // FIPS-197 Appendix C.1
  uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  uint8_t in[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  uint8_t out[16];
  crypto::aes128_encrypt_block(key, in, out);
  uint8_t expect[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                        0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
  PB_CHECK(std::equal(out, out + 16, expect));
}

PB_TEST(aes_fips197_appendix_b_vector) {
  // FIPS-197 Appendix B（全 00 密钥）
  uint8_t key[16] = {0};
  uint8_t in[16] = {0};
  uint8_t out[16];
  crypto::aes128_encrypt_block(key, in, out);
  // e(k,0) = 66E94BD4EF8A2C3B884CFA59CA342B2E? —— 这是密钥全 0 的著名结果（SP800-38A 附录）
  uint8_t expect[16] = {0x66, 0xE9, 0x4B, 0xD4, 0xEF, 0x8A, 0x2C, 0x3B,
                        0x88, 0x4C, 0xFA, 0x59, 0xCA, 0x34, 0x2B, 0x2E};
  PB_CHECK(std::equal(out, out + 16, expect));
}

PB_TEST(cmac_rfc4493_empty_message) {
  // RFC 4493 §4 Example 1（Mlen=0）
  uint8_t key[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                     0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
  uint8_t out[16];
  crypto::aes_cmac(key, nullptr, 0, out);
  uint8_t expect[16] = {0xBB, 0x1D, 0x69, 0x29, 0xE9, 0x59, 0x37, 0x28,
                        0x7F, 0xA3, 0x7D, 0x12, 0x9B, 0x75, 0x67, 0x46};
  PB_CHECK(std::equal(out, out + 16, expect));
}

PB_TEST(cmac_rfc4493_full_block) {
  uint8_t key[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                     0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
  uint8_t m[16] = {0x6B, 0xC1, 0xBE, 0xE2, 0x2E, 0x40, 0x9F, 0x96,
                   0xE9, 0x3D, 0x7E, 0x11, 0x73, 0x93, 0x17, 0x2A};
  uint8_t out[16];
  crypto::aes_cmac(key, m, 16, out);
  uint8_t expect[16] = {0x07, 0x0A, 0x16, 0xB4, 0x6B, 0x4D, 0x41, 0x44,
                        0xF7, 0x9B, 0xDD, 0x9D, 0xD0, 0x4A, 0x28, 0x7C};
  PB_CHECK(std::equal(out, out + 16, expect));
}

PB_TEST(cmac_rfc4493_40_bytes) {
  uint8_t key[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                     0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
  uint8_t m[40] = {0x6B, 0xC1, 0xBE, 0xE2, 0x2E, 0x40, 0x9F, 0x96, 0xE9, 0x3D,
                   0x7E, 0x11, 0x73, 0x93, 0x17, 0x2A, 0xAE, 0x2D, 0x8A, 0x57,
                   0x1E, 0x03, 0xAC, 0x9C, 0x9E, 0xB7, 0x6F, 0xAC, 0x45, 0xAF,
                   0x8E, 0x51, 0x30, 0xC8, 0x1C, 0x46, 0xA3, 0x5C, 0xE4, 0x11};
  uint8_t out[16];
  crypto::aes_cmac(key, m, 40, out);
  uint8_t expect[16] = {0xDF, 0xA6, 0x67, 0x47, 0xDE, 0x9A, 0xE6, 0x30,
                        0x30, 0xCA, 0x32, 0x61, 0x14, 0x97, 0xC8, 0x27};
  PB_CHECK(std::equal(out, out + 16, expect));
}

// —— SMP ——
PB_TEST(smp_s1_definition) {
  // s1(k,r1,r2)=e(k, r1'⊕r2')，r'=低 64bit（高 64bit 清零）
  uint8_t k[16], r1[16], r2[16];
  for (int i = 0; i < 16; ++i) {
    k[i] = (uint8_t)i;
    r1[i] = (uint8_t)(0x10 + i);
    r2[i] = (uint8_t)(0x20 + i);
  }
  auto out = ble::SmpResponder::s1(k, r1, r2);
  uint8_t in[16] = {0};
  for (int i = 0; i < 8; ++i) in[i] = (uint8_t)(r1[8 + i] ^ r2[8 + i]);
  uint8_t direct[16];
  crypto::aes128_encrypt_block(k, in, direct);
  PB_CHECK(std::equal(out.begin(), out.end(), direct));
}

PB_TEST(smp_just_works_exchange) {
  ble::SmpResponder smp;
  PB_CHECK(!smp.paired());

  // 1) Pairing Request（io=3 oob=0 auth=0 max=16 init=3）
  uint8_t preq[7] = {0x01, 0x03, 0x00, 0x00, 0x10, 0x03, 0x00};
  auto pres = smp.handle(preq, 7);
  PB_CHECK(pres.has_value());
  PB_CHECK_EQ((*pres)[0], 0x02);  // Pairing Response
  PB_CHECK_EQ((*pres)[1], 0x03);  // NoInputNoOutput → Just Works
  PB_CHECK(!smp.paired());

  // 2) Pairing Confirm（主机侧值不影响我方应答计算）
  uint8_t confirm[17] = {0x03};
  for (int i = 0; i < 16; ++i) confirm[1 + i] = (uint8_t)(0xA0 + i);
  auto crsp = smp.handle(confirm, 17);
  PB_CHECK(crsp.has_value());
  PB_CHECK_EQ((*crsp)[0], 0x03);  // Pairing Confirm

  // 3) Pairing Random → Random + paired
  uint8_t random[17] = {0x04};
  for (int i = 0; i < 16; ++i) random[1 + i] = (uint8_t)(0x50 + i);
  auto rrsp = smp.handle(random, 17);
  PB_CHECK(rrsp.has_value());
  PB_CHECK_EQ((*rrsp)[0], 0x05);  // Pairing Random
  PB_CHECK(smp.paired());

  // STK = s1(TK=0, Srand, Mrand) 独立复算
  uint8_t tk[16] = {0};
  uint8_t srand[16], mrand[16];
  for (int i = 0; i < 16; ++i) {
    srand[i] = (uint8_t)(0x10 + i);
    mrand[i] = (uint8_t)(0x50 + i);
  }
  auto expect = ble::SmpResponder::s1(tk, srand, mrand);
  PB_CHECK(std::equal(expect.begin(), expect.end(), smp.stk()));
}

PB_TEST(smp_c1_definition) {
  // c1 双重结构与规范一致（独立构造验证实现自洽）
  uint8_t tk[16], rand[16], preq[7], pres[7], ia[6], ra[6];
  for (int i = 0; i < 16; ++i) {
    tk[i] = (uint8_t)(i * 3);
    rand[i] = (uint8_t)(i * 7 + 1);
  }
  for (int i = 0; i < 7; ++i) {
    preq[i] = (uint8_t)(0x10 + i);
    pres[i] = (uint8_t)(0x20 + i);
  }
  for (int i = 0; i < 6; ++i) {
    ia[i] = (uint8_t)(0x30 + i);
    ra[i] = (uint8_t)(0x40 + i);
  }
  auto out = ble::SmpResponder::c1(tk, rand, preq, pres, 0, ia, 1, ra);
  // 手工两步复算
  uint8_t r[16];
  std::memcpy(r, rand, 16);
  for (int i = 0; i < 7; ++i) r[i] ^= preq[i];
  for (int i = 0; i < 7; ++i) r[7 + i] ^= pres[i];
  r[14] ^= 0;
  r[15] ^= 1;
  uint8_t p1[16];
  crypto::aes128_encrypt_block(tk, r, p1);
  uint8_t p2in[16] = {0};
  for (int i = 0; i < 6; ++i) {
    p2in[i] = ia[5 - i];
    p2in[6 + i] = ra[5 - i];
  }
  uint8_t x[16], fin[16];
  for (int i = 0; i < 16; ++i) x[i] = p1[i] ^ p2in[i];
  crypto::aes128_encrypt_block(tk, x, fin);
  PB_CHECK(std::equal(out.begin(), out.end(), fin));
}
