#include "test/test_framework.h"
#include "usbip/protocol.h"

#include "core/bytes.h"

using namespace pb;
using namespace pb::usbip;

static ExportedDevice sample() {
  ExportedDevice d;
  d.path = "/phantom/7-1";
  d.busid = "7-1";
  d.vid = 0x1234;
  d.pid = 0x5678;
  d.dev_class = 0;
  d.num_interfaces = 1;
  d.interfaces.push_back({3, 0, 0});
  return d;
}

// usbip_usb_device 字段偏移（312B 布局）
constexpr size_t OFF_PATH = 0;        // 256
constexpr size_t OFF_BUSID = 256;     // 32
constexpr size_t OFF_BUSNUM = 288;    // u32
constexpr size_t OFF_DEVNUM = 292;    // u32
constexpr size_t OFF_SPEED = 296;     // u32
constexpr size_t OFF_VID = 300;       // u16
constexpr size_t OFF_PID = 302;       // u16
constexpr size_t OFF_BCD = 304;       // u16
constexpr size_t OFF_CLASS = 306;     // u8×6

PB_TEST(usbip_usb_device_packed_size) {
  auto b = pack_usb_device(sample());
  PB_CHECK_EQ(b.size(), 312u);
}

PB_TEST(usbip_rep_devlist_layout) {
  auto b = pack_rep_devlist({sample()});
  PB_CHECK_EQ(b.size(), 12u + 312u + 4u);  // 头12 + 设备312 + 接口4
  PB_CHECK_EQ(b[0], 0x00);
  PB_CHECK_EQ(b[3], 0x01);   // OP_REP_DEVLIST BE
  PB_CHECK_EQ(b[7], 0x00);   // status=0
  PB_CHECK_EQ(b[11], 0x01);  // ndev=1
  size_t d = 12;             // 设备块起点
  PB_CHECK_EQ(b[d + OFF_PATH], '/');
  PB_CHECK_EQ(b[d + OFF_PATH + 1], 'p');
  PB_CHECK_EQ(b[d + OFF_BUSID], '7');
  PB_CHECK_EQ(b[d + OFF_BUSID + 2], '1');
  PB_CHECK_EQ(b[d + OFF_BUSNUM + 3], 7);        // busnum BE
  PB_CHECK_EQ(b[d + OFF_DEVNUM + 3], 1);        // devnum BE
  PB_CHECK_EQ(b[d + OFF_SPEED + 3], 2);         // speed FULL
  PB_CHECK_EQ(b[d + OFF_VID], 0x12);            // idVendor BE u16
  PB_CHECK_EQ(b[d + OFF_VID + 1], 0x34);
  PB_CHECK_EQ(b[d + OFF_PID], 0x56);
  PB_CHECK_EQ(b[d + OFF_PID + 1], 0x78);
  PB_CHECK_EQ(b[d + OFF_CLASS], 0);             // class
  PB_CHECK_EQ(b[d + OFF_CLASS + 5], 1);         // num_interfaces
  size_t i = d + 312;  // 接口块
  PB_CHECK_EQ(b[i], 3);       // bInterfaceClass
  PB_CHECK_EQ(b[i + 3], 0);   // padding
}

PB_TEST(usbip_rep_import_layout) {
  auto b = pack_rep_import(sample());
  PB_CHECK_EQ(b.size(), 8u + 312u);
  PB_CHECK_EQ(b[3], 0x04);  // OP_REP_IMPORT
  PB_CHECK_EQ(b[7], 0x00);  // status=0
  PB_CHECK_EQ(b[8 + OFF_VID], 0x12);
}

PB_TEST(usbip_submit_roundtrip) {
  ByteWriter w;
  w.u32be(USBIP_CMD_SUBMIT);
  w.u32be(0x42);
  w.u32be(0x00070001);
  w.u32be(DIR_IN);
  w.u32be(0x81);
  w.u32be(0);   // transfer_flags
  w.u32be(8);   // transfer_buffer_length
  w.u32be(0);   // start_frame
  w.u32be(0);   // number_of_packets
  w.u32be(10);  // interval
  uint8_t setup[8] = {0x81, 6, 0, 1, 0, 0, 2, 0};
  w.bytes(setup, 8);
  SubmitHeader h;
  PB_CHECK(parse_submit(w.buf().data(), w.buf().size(), h));
  PB_CHECK_EQ(h.seqnum, 0x42u);
  PB_CHECK_EQ(h.devid, 0x00070001u);
  PB_CHECK_EQ(h.ep, 0x81u);
  PB_CHECK_EQ(h.setup[0], 0x81);
  PB_CHECK_EQ(h.setup[1], 6);
  PB_CHECK_EQ(h.transfer_buffer_length, 8);
  PB_CHECK_EQ(h.interval, 10);
  // 截断拒绝
  PB_CHECK(!parse_submit(w.buf().data(), 47, h));
  // 非 CMD_SUBMIT 拒绝
  auto mutated = w.buf();
  mutated[3] = 0x02;
  PB_CHECK(!parse_submit(mutated.data(), mutated.size(), h));
}

PB_TEST(usbip_unlink_roundtrip) {
  ByteWriter w;
  w.u32be(USBIP_CMD_UNLINK);
  w.u32be(5);         // seqnum
  w.u32be(0x00070001);
  w.u32be(DIR_OUT);
  w.u32be(0x81);      // ep
  w.u32be(9);         // unlink_seqnum
  uint32_t seq, devid, ep, unlink;
  PB_CHECK(parse_unlink(w.buf().data(), w.buf().size(), seq, devid, ep, unlink));
  PB_CHECK_EQ(seq, 5u);
  PB_CHECK_EQ(unlink, 9u);
  PB_CHECK_EQ(ep, 0x81u);
  PB_CHECK(!parse_unlink(w.buf().data(), 20, seq, devid, ep, unlink));
}

PB_TEST(usbip_ret_submit_in_carries_data) {
  uint8_t data[] = {0xAA, 0xBB};
  auto b = pack_ret_submit(7, 1, 0x81, DIR_IN, 0, 2, data, 2);
  PB_CHECK_EQ(b.size(), 42u);
  PB_CHECK_EQ(b[3], 0x03);  // RET_SUBMIT
  PB_CHECK_EQ(b[7], 7);     // seqnum BE
  PB_CHECK_EQ(b[27], 2);    // actual_length BE（basic20+status4=24, +4=28 → offset 24..27）
  PB_CHECK_EQ(b[40], 0xAA);
  PB_CHECK_EQ(b[41], 0xBB);
  // OUT 方向不带 data
  auto b2 = pack_ret_submit(7, 1, 0x02, DIR_OUT, 0, 2, data, 2);
  PB_CHECK_EQ(b2.size(), 40u);
}

PB_TEST(usbip_ret_unlink) {
  auto b = pack_ret_unlink(7, 1, 0x81, 0);
  PB_CHECK_EQ(b.size(), 24u);
  PB_CHECK_EQ(b[3], 0x04);
}

PB_TEST(usbip_import_busid_parse) {
  uint8_t body[32] = {};
  std::memcpy(body, "7-1", 3);
  std::string busid;
  PB_CHECK(parse_import_busid(body, sizeof(body), busid));
  PB_CHECK_EQ(busid, std::string("7-1"));
  uint8_t empty[32] = {};
  PB_CHECK(!parse_import_busid(empty, sizeof(empty), busid));
}

PB_TEST(usbip_op_header) {
  auto b = pack_op_header(OP_REP_IMPORT, 1);
  PB_CHECK_EQ(b.size(), 8u);
  PB_CHECK_EQ(b[3], 0x04);
  PB_CHECK_EQ(b[7], 0x01);
}
