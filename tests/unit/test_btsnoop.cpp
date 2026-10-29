#include "test/test_framework.h"
#include "sim/btsnoop.h"

#include <cstdio>

#include "core/bytes.h"
#include "hci/packets.h"

using namespace pb;

PB_TEST(btsnoop_header_golden) {
  sim::BtSnoopWriter w;
  std::string path = "integration-results/test.snoop";
  PB_CHECK(w.open(path));
  PB_CHECK(w.is_open());
  w.close();

  FILE* f = std::fopen(path.c_str(), "rb");
  PB_CHECK(f != nullptr);
  uint8_t h[16];
  PB_CHECK_EQ(std::fread(h, 1, 16, f), 16u);
  static const uint8_t expect[16] = {'b', 't', 's', 'n', 'o', 'o', 'p', 0,
                                     0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03, 0xEA};
  PB_CHECK(std::equal(h, h + 16, expect));
  std::fclose(f);
}

PB_TEST(btsnoop_record_layout) {
  sim::BtSnoopWriter w;
  std::string path = "integration-results/test2.snoop";
  PB_CHECK(w.open(path));
  // HCI Reset 命令 → 记录 = 24 头 + 4(H4+cmd3)
  w.hci_command({0x03, 0x0C, 0x00});
  PB_CHECK_EQ(w.record_count(), 1u);
  // 事件
  w.hci_event(hci::build_command_complete(1, hci::CMD_RESET, {0x00}));
  // ACL 双向
  hci::AclFrame fr{1, hci::ACL_PB_START, {1, 2}};
  w.acl_from_host(fr);
  w.acl_to_host(fr);
  PB_CHECK_EQ(w.record_count(), 4u);
  w.close();

  FILE* f = std::fopen(path.c_str(), "rb");
  PB_CHECK(f != nullptr);
  std::fseek(f, 16, SEEK_SET);  // 跳过文件头
  uint8_t rec[24 + 4];
  PB_CHECK_EQ(std::fread(rec, 1, 28, f), 28u);
  // orig_len=4 BE
  PB_CHECK_EQ(rec[3], 4u);
  // incl_len=4
  PB_CHECK_EQ(rec[7], 4u);
  // H4 type 0x01 + Reset
  PB_CHECK_EQ(rec[24], 0x01);
  PB_CHECK_EQ(rec[25], 0x03);
  PB_CHECK_EQ(rec[26], 0x0C);
  PB_CHECK_EQ(rec[27], 0x00);
  std::fclose(f);
}

PB_TEST(btsnoop_closed_writer_noop) {
  sim::BtSnoopWriter w;
  // 未打开：写入静默忽略
  w.hci_command({0x03, 0x0C, 0x00});
  PB_CHECK_EQ(w.record_count(), 0u);
  PB_CHECK(!w.is_open());
}
