#include "test/test_framework.h"
#include "core/bytes.h"

PB_TEST(bytes_writer_endian) {
  pb::ByteWriter w;
  w.u16le(0x1234);
  w.u16be(0x1234);
  w.u32be(0x01020304);
  PB_CHECK_EQ(pb::to_hex(w.buf()), std::string("3412123401020304"));
}

PB_TEST(bytes_writer_u64be_and_zeros) {
  pb::ByteWriter w;
  w.u64be(0x0102030405060708ull);
  w.zeros(2);
  w.u8(0xFF);
  PB_CHECK_EQ(pb::to_hex(w.buf()), std::string("0102030405060708" "0000" "ff"));
}

PB_TEST(bytes_reader_overrun_sets_error) {
  uint8_t d[] = {1, 2};
  pb::ByteReader r(d, 2);
  PB_CHECK_EQ(r.u16be(), 0x0102);
  PB_CHECK(r.ok());
  PB_CHECK_EQ(r.remaining(), 0u);
  PB_CHECK_EQ(r.u8(), 0);  // 无剩余字节 → 进入 error 态
  PB_CHECK(!r.ok());
  PB_CHECK_EQ(r.u8(), 0);  // error 态保持
  PB_CHECK(!r.ok());
}

PB_TEST(bytes_reader_le_and_skip) {
  std::vector<uint8_t> v = {0x78, 0x56, 0x34, 0x12, 0xAA, 0xBB};
  pb::ByteReader r(v);
  PB_CHECK_EQ(r.u16le(), 0x5678);
  PB_CHECK_EQ(r.u32le(), 0xBBAA1234u);
  pb::ByteReader r2(v);
  r2.skip(4);
  PB_CHECK_EQ(r2.u16le(), 0xBBAA);
}

PB_TEST(bytes_from_hex_rejects_odd_and_bad) {
  PB_CHECK_THROWS(pb::from_hex("abc"));
  PB_CHECK_THROWS(pb::from_hex("0g"));
  auto v = pb::from_hex("0a0B");
  PB_CHECK_EQ(v.size(), 2u);
  PB_CHECK_EQ(v[0], 0x0A);
  PB_CHECK_EQ(v[1], 0x0B);
}

PB_TEST(bytes_roundtrip_take) {
  pb::ByteWriter w;
  w.u32be(0xDEADBEEF);
  auto v = w.take();
  PB_CHECK_EQ(v.size(), 4u);
  PB_CHECK_EQ(w.size(), 0u);
  pb::ByteReader r(v.data(), v.size());
  PB_CHECK_EQ(r.u32be(), 0xDEADBEEFu);
}
