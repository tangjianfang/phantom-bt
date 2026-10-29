#include "test/test_framework.h"
#include "hci/controller.h"
#include "hci/packets.h"

using namespace pb;
using namespace pb::hci;

// 测试辅助：发命令并丢弃 Command Complete/Status
static void drain_cc(MiniController& c) {
  std::vector<uint8_t> e;
  c.pop_event(e);
}

PB_TEST(controller_scan_enable_streams_adv_events) {
  MiniController c(ControllerConfig{});
  auto d = std::make_shared<VirtualAdvertiser>(0xC01122334455ull, "PhantomKb");
  d->set_adv_data_name("PhantomKb");
  c.air().add(d);

  // 扫描关：tick 不产事件
  c.tick_for_tests(0);
  c.tick_for_tests(200);
  std::vector<uint8_t> e;
  PB_CHECK(!c.pop_event(e));

  // LE_Set_Scan_Enable(1, no dup filter)
  uint8_t enable[] = {0x0C, 0x20, 0x02, 0x01, 0x00};
  c.handle_command(enable, 5);
  drain_cc(c);  // Command Complete
  PB_CHECK(c.le_scan_enabled());

  c.tick_for_tests(0);      // 首次立即发报
  c.tick_for_tests(50);     // <128ms 不发
  c.tick_for_tests(200);    // ≥128ms 发
  int adv = 0;
  while (c.pop_event(e)) {
    if (e[0] == EVT_LE_META && e[2] == LE_EVT_ADVERTISING_REPORT) ++adv;
  }
  PB_CHECK(adv >= 2);

  // 关扫描后不再发
  uint8_t disable[] = {0x0C, 0x20, 0x02, 0x00, 0x00};
  c.handle_command(disable, 5);
  drain_cc(c);
  c.tick_for_tests(500);
  c.tick_for_tests(1000);
  adv = 0;
  while (c.pop_event(e)) ++adv;
  PB_CHECK_EQ(adv, 0);
}

PB_TEST(controller_le_create_connection_completes) {
  MiniController c(ControllerConfig{});
  auto d = std::make_shared<VirtualAdvertiser>(0xC01122334455ull, "PhantomKb");
  c.air().add(d);

  // LE_Create_Connection：scan_int(2) scan_win(2) filter(1) type(1) addr(6) 其余 12B
  std::vector<uint8_t> cmd = {0x0D, 0x20, 24};
  cmd.push_back(0x60); cmd.push_back(0x00);  // scan interval
  cmd.push_back(0x30); cmd.push_back(0x00);  // scan window
  cmd.push_back(0x00);                       // filter policy
  cmd.push_back(0x01);                       // peer addr type = random
  for (int i = 0; i < 6; ++i)                // peer addr LE
    cmd.push_back((uint8_t)((0xC01122334455ull >> (8 * i)) & 0xFF));
  for (int i = 0; i < 12; ++i) cmd.push_back(0);  // interval×2/latency/timeout/ce×2
  c.handle_command(cmd.data(), cmd.size());

  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));  // Command Status
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[2], ST_SUCCESS);
  PB_CHECK(!c.has_connection());

  // 未到延迟：tick 若干次
  for (int i = 0; i < 5; ++i) c.tick_for_tests(i * 16);
  while (c.pop_event(e)) PB_CHECK(!"unexpected event before delay");  // 无广播扫描 → 仅连接事件

  for (int i = 5; i < 20; ++i) c.tick_for_tests(i * 16);  // 越过 ~208ms
  bool got_conn = false;
  while (c.pop_event(e)) {
    if (e[0] == EVT_LE_META && e[2] == LE_EVT_CONNECTION_COMPLETE) {
      got_conn = true;
      PB_CHECK_EQ(e[3], ST_SUCCESS);   // status
      PB_CHECK_EQ(e[4], 0x01);         // handle LSB
      PB_CHECK_EQ(e[5], 0x00);
      PB_CHECK_EQ(e[6], 0x00);         // role central
      PB_CHECK_EQ(e[7], 0x01);         // addr type random
      PB_CHECK_EQ(e[8], 0x55);         // addr LSB
      PB_CHECK_EQ(e[13], 0xC0);        // addr MSB（e[8..13] LSB first）
    }
  }
  PB_CHECK(got_conn);
  PB_CHECK(c.has_connection());
  PB_CHECK_EQ(c.connection_handle(), 0x0001);
  PB_CHECK_EQ(c.connection_peer(), 0xC01122334455ull);
}

PB_TEST(controller_le_create_connection_unknown_peer_fails) {
  MiniController c(ControllerConfig{});
  std::vector<uint8_t> cmd = {0x0D, 0x20, 24, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
  for (int i = 0; i < 6; ++i) cmd.push_back(0x99);  // 不存在的地址
  for (int i = 0; i < 12; ++i) cmd.push_back(0);
  c.handle_command(cmd.data(), cmd.size());
  std::vector<uint8_t> e;
  c.pop_event(e);  // CS
  for (int i = 0; i < 30; ++i) c.tick_for_tests(i * 16);
  bool got_fail = false;
  while (c.pop_event(e)) {
    if (e[0] == EVT_LE_META && e[2] == LE_EVT_CONNECTION_COMPLETE) {
      got_fail = true;
      PB_CHECK_EQ(e[3], ST_UNKNOWN_CONN_ID);
    }
  }
  PB_CHECK(got_fail);
  PB_CHECK(!c.has_connection());
}

PB_TEST(controller_disconnect_event) {
  MiniController c(ControllerConfig{});
  auto d = std::make_shared<VirtualAdvertiser>(0xC00000000001ull, "x");
  c.air().add(d);
  std::vector<uint8_t> cmd = {0x0D, 0x20, 24,
                               0x60, 0x00,             // scan interval
                               0x30, 0x00,             // scan window
                               0x00,                   // filter
                               0x01,                   // peer type random
                               0x01, 0x00, 0x00, 0x00, 0x00, 0xC0};  // peer 0xC00000000001（LSB first）
  for (int i = 0; i < 12; ++i) cmd.push_back(0);
  c.handle_command(cmd.data(), cmd.size());
  std::vector<uint8_t> e;
  c.pop_event(e);
  for (int i = 0; i < 30; ++i) c.tick_for_tests(i * 16);
  PB_CHECK(c.has_connection());

  c.simulate_disconnect(0x13);  // Remote User Terminated
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_DISCONNECTION_COMPLETE);
  PB_CHECK_EQ(e[2], ST_SUCCESS);  // status
  PB_CHECK_EQ(e[3], 0x01);        // handle LSB（e[3..4]）
  PB_CHECK_EQ(e[5], 0x13);        // reason
  PB_CHECK(!c.has_connection());
}
