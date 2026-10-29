#include "test/test_framework.h"
#include "hci/controller.h"

#include "core/bytes.h"

using namespace pb;
using namespace pb::hci;

static void send(MiniController& c, const std::vector<uint8_t>& cmd) {
  c.handle_command(cmd.data(), cmd.size());
}

PB_TEST(controller_reset_bd_addr_radio_ready) {
  MiniController c(ControllerConfig{});
  send(c, {0x03, 0x0C, 0x00});  // Reset
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
  PB_CHECK_EQ(e[1], 4u);  // nump+op2+status
  PB_CHECK_EQ(e[3], 0x03);  // opcode LE 低字节在前
  PB_CHECK_EQ(e[4], 0x0C);
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK(!c.radio_ready());

  send(c, {0x09, 0x10, 0x00});  // Read_BD_ADDR
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[1], 10u);  // nump+op+status+addr6
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK_EQ(e[6], 0x11);
  PB_CHECK_EQ(e[11], 0x66);
  PB_CHECK(c.radio_ready());
}

PB_TEST(controller_unknown_vendor_logged) {
  MiniController c(ControllerConfig{});
  send(c, {0x00, 0xFC, 0x00});  // vendor opcode 0xFC00
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
  PB_CHECK_EQ(e[5], ST_UNKNOWN_CMD);  // return params 首字节
  PB_CHECK_EQ(c.unknown_opcodes_logged(), 1);
  auto log = c.op_log();
  PB_CHECK_EQ(log.back(), std::string("op=0xFC00(?) st=01 UNKNOWN"));
}

PB_TEST(controller_le_buffer_size_params) {
  MiniController c(ControllerConfig{});
  send(c, {0x02, 0x20, 0x00});  // LE_Read_Buffer_Size
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  // 0E 08 01 02 20 00 FB 00 00 08：status, le_acl_mtu LE(2), sco_mtu(1), num(1)
  PB_CHECK_EQ(e[1], 8u);
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK_EQ(e[6], 0xFB);
  PB_CHECK_EQ(e[7], 0x00);
  PB_CHECK_EQ(e[8], 0u);  // 无 SCO
  PB_CHECK_EQ(e[9], 8u);
}

PB_TEST(controller_read_bd_addr_layout) {
  ControllerConfig cfg;
  cfg.bd_addr = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  MiniController c(cfg);
  send(c, {0x09, 0x10, 0x00});
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[6], 0xAA);
  PB_CHECK_EQ(e[11], 0xFF);
}

PB_TEST(controller_local_name_write_read) {
  MiniController c(ControllerConfig{});
  std::vector<uint8_t> cmd = {0x13, 0x0C, (uint8_t)248};
  for (int i = 0; i < 248; ++i) cmd.push_back(i == 0 ? 'X' : 0);
  send(c, cmd);
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[5], ST_SUCCESS);

  send(c, {0x14, 0x0C, 0x00});  // Read_Local_Name
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[1], 252u);  // nump+op+status+248
  PB_CHECK_EQ(e[6], 'X');
}

PB_TEST(controller_supported_commands_bitmap_has_supported) {
  MiniController c(ControllerConfig{});
  send(c, {0x02, 0x10, 0x00});  // Read_Local_Supported_Commands
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[1], 68u);  // nump+op+status+64
  // Reset (ogf3, ocf3): pos=2*64+3=131 → octet16 bit3
  size_t pos = supported_commands_bit(CMD_RESET);
  PB_CHECK_EQ(e[6 + pos / 8] & (1 << (pos % 8)), (1u << (pos % 8)));
  // LE_Set_Scan_Enable (ogf8, ocf0x0C): pos=7*64+12=460 → octet 57
  pos = supported_commands_bit(CMD_LE_SET_SCAN_ENABLE);
  PB_CHECK(e[6 + pos / 8] & (1 << (pos % 8)));
  // Vendor (ogf0x3F) 不在表内
  pos = supported_commands_bit(0xFC00);
  PB_CHECK(!(e[6 + pos / 8] & (1 << (pos % 8))));
}

PB_TEST(controller_features_flags) {
  MiniController c(ControllerConfig{});
  send(c, {0x03, 0x10, 0x00});  // Read_Local_Supported_Features
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[1], 12u);  // nump+op+status+8
  PB_CHECK_EQ(e[6], 0xFF);   // byte0：加密等
  PB_CHECK_EQ(e[10], 0xDB);  // byte4 bit6 = LE supported

  send(c, {0x03, 0x20, 0x00});  // LE_Read_Local_Supported_Features
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[6], 0x01);  // LE encryption
}

PB_TEST(controller_scan_enable_roundtrip) {
  MiniController c(ControllerConfig{});
  send(c, {0x1A, 0x0C, 0x01, 0x03});  // Write_Scan_Enable(3)
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK_EQ(c.scan_enable(), 3);
  send(c, {0x19, 0x0C, 0x00});  // Read_Scan_Enable
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[6], 3);
}

PB_TEST(controller_le_scan_enable_state) {
  MiniController c(ControllerConfig{});
  send(c, {0x0C, 0x20, 0x02, 0x01, 0x00});  // LE_Set_Scan_Enable(enable=1)
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK(c.le_scan_enabled());
  send(c, {0x0C, 0x20, 0x02, 0x00, 0x00});
  PB_CHECK(c.pop_event(e));
  PB_CHECK(!c.le_scan_enabled());
}

PB_TEST(controller_create_connection_returns_status_pending) {
  MiniController c(ControllerConfig{});
  // LE_Create_Connection 最少 25 字节参数
  std::vector<uint8_t> cmd = {0x0D, 0x20, 25};
  for (int i = 0; i < 25; ++i) cmd.push_back(0);
  send(c, cmd);
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[2], ST_SUCCESS);  // status 字段（event params 首字节）
}

PB_TEST(controller_event_count_and_oplog) {
  MiniController c(ControllerConfig{});
  send(c, {0x03, 0x0C, 0x00});
  send(c, {0x09, 0x10, 0x00});
  send(c, {0x02, 0x20, 0x00});
  PB_CHECK_EQ(c.event_count(), 3u);
  auto log = c.op_log();
  PB_CHECK_EQ(log.size(), 3u);
  PB_CHECK(log[0].find("Reset") != std::string::npos);
  PB_CHECK_EQ(c.unknown_opcodes_logged(), 0);
}
