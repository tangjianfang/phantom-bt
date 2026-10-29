#include "test/test_framework.h"
#include "hci/packets.h"

#include "core/bytes.h"

using namespace pb;
using namespace pb::hci;

PB_TEST(hci_opcode_compose) {
  PB_CHECK_EQ(opcode(3, 0x0003), 0x0C03);   // Reset
  PB_CHECK_EQ(opcode(4, 0x0009), 0x1009);   // Read_BD_ADDR
  PB_CHECK_EQ(opcode(8, 0x000D), 0x200D);   // LE_Create_Connection
  PB_CHECK_EQ(opcode(0x3F, 0), 0xFC00);     // Vendor
  PB_CHECK_EQ(ogf_of(0x200D), 0x08u);
  PB_CHECK_EQ(ocf_of(0x200D), 0x000Du);
}

PB_TEST(hci_parse_command) {
  uint8_t p[] = {0x03, 0x0C, 0x00};  // Reset
  HciCommand c;
  PB_CHECK(parse_command(p, 3, c));
  PB_CHECK_EQ(c.op, CMD_RESET);
  PB_CHECK_EQ(c.params.size(), 0u);

  uint8_t bad[] = {0x03, 0x0C, 0x02, 0xAA};  // len=2 但只有 1 字节参数
  PB_CHECK(!parse_command(bad, 4, c));
  PB_CHECK(!parse_command(p, 2, c));
}

PB_TEST(hci_command_complete_layout) {
  auto e = build_command_complete(1, CMD_RESET, {ST_SUCCESS});
  static const uint8_t expect[] = {0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00};
  PB_CHECK_EQ(e.size(), sizeof(expect));
  PB_CHECK(std::equal(e.begin(), e.end(), expect));
}

PB_TEST(hci_command_status_layout) {
  auto e = build_command_status(ST_SUCCESS, 1, CMD_LE_CREATE_CONNECTION);
  // 0F 04 00 01 0D 20
  PB_CHECK_EQ(e[0], 0x0Fu);
  PB_CHECK_EQ(e[2], 0x00u);  // status
  PB_CHECK_EQ(e[4], 0x0Du);
  PB_CHECK_EQ(e[5], 0x20u);
}

PB_TEST(hci_le_meta_wrapped) {
  auto e = build_le_meta(LE_EVT_ADVERTISING_REPORT, {0x01, 0x00});
  PB_CHECK_EQ(e[0], EVT_LE_META);
  PB_CHECK_EQ(e[1], 3u);  // subevent + 2
  PB_CHECK_EQ(e[2], LE_EVT_ADVERTISING_REPORT);
}

PB_TEST(hci_acl_roundtrip) {
  AclFrame f;
  f.handle = 0x0005;
  f.pb = ACL_PB_START;
  f.data = {1, 2, 3};
  auto a = build_acl(f);
  PB_CHECK_EQ(a.size(), 7u);
  PB_CHECK_EQ(a[0], 0x05);
  PB_CHECK_EQ(a[1], 0x00);  // handle | PB_START
  PB_CHECK_EQ(a[2], 3u);
  AclFrame g;
  PB_CHECK(parse_acl(a.data(), a.size(), g));
  PB_CHECK_EQ(g.handle, 5u);
  PB_CHECK_EQ(g.pb, ACL_PB_START);
  PB_CHECK_EQ(g.data.size(), 3u);

  // PB_CONT 分片
  AclFrame h{5, ACL_PB_CONT, {9}};
  auto a2 = build_acl(h);
  PB_CHECK_EQ(a2[1], 0x10);
  // 长度字段撒谎 → 拒绝
  uint8_t bad[] = {0x05, 0x00, 0x10, 0x00, 1};
  PB_CHECK(!parse_acl(bad, sizeof(bad), g));
}

PB_TEST(hci_build_command_roundtrip) {
  auto v = build_command(CMD_LE_SET_SCAN_ENABLE, {0x01, 0x00});
  HciCommand c;
  PB_CHECK(parse_command(v.data(), v.size(), c));
  PB_CHECK_EQ(c.op, CMD_LE_SET_SCAN_ENABLE);
  PB_CHECK_EQ(c.params.size(), 2u);
  PB_CHECK_EQ(c.params[0], 0x01);
}
