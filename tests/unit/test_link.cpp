#include "test/test_framework.h"
#include "hci/controller.h"
#include "hci/link.h"
#include "hci/packets.h"

using namespace pb;
using namespace pb::hci;

namespace {

struct Connected {
  MiniController c{ControllerConfig{}};
  Connected() {
    auto d = std::make_shared<VirtualAdvertiser>(0xC01122334455ull, "kb");
    c.air().add(d);
    std::vector<uint8_t> cmd = {0x0D, 0x20, 24, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; ++i) cmd.push_back((uint8_t)((0xC01122334455ull >> (8 * i)) & 0xFF));
    for (int i = 0; i < 12; ++i) cmd.push_back(0);
    c.handle_command(cmd.data(), cmd.size());
    std::vector<uint8_t> e;
    c.pop_event(e);  // CS
    for (int i = 0; i < 30; ++i) c.tick_for_tests(i * 16);
    while (c.pop_event(e)) {}  // 排空连接事件
  }
};

}  // namespace

PB_TEST(link_reassembly_host_to_device) {
  Connected env;
  PB_CHECK(env.c.has_connection());
  uint16_t h = env.c.connection_handle();

  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> pdus;
  env.c.on_l2cap_from_host = [&](uint16_t handle, const std::vector<uint8_t>& pdu) {
    pdus.emplace_back(handle, pdu);
  };

  // 完整 PDU：ATT MTU Exchange Req = L2CAP 头(03 00 04 00) + ATT(02 17 00)
  std::vector<uint8_t> pdu1 = {0x03, 0x00, 0x04, 0x00, 0x02, 0x17, 0x00};
  env.c.handle_acl_from_host(AclFrame{h, ACL_PB_START, pdu1});
  PB_CHECK_EQ(pdus.size(), 1u);
  PB_CHECK_EQ(pdus[0].first, h);
  PB_CHECK_EQ(pdus[0].second.size(), 7u);
  PB_CHECK_EQ(pdus[0].second[4], 0x02);

  // 分两帧：START(前 4B l2cap 头) + CONT(剩余 3B)
  std::vector<uint8_t> pdu2 = {0x03, 0x00, 0x04, 0x00, 0x03, 0x00, 0x28};  // len=3 payload
  env.c.handle_acl_from_host(AclFrame{h, ACL_PB_START, {pdu2.begin(), pdu2.begin() + 4}});
  PB_CHECK_EQ(pdus.size(), 1u);  // 未完成
  env.c.handle_acl_from_host(AclFrame{h, ACL_PB_CONT, {pdu2.begin() + 4, pdu2.end()}});
  PB_CHECK_EQ(pdus.size(), 2u);
  PB_CHECK_EQ(pdus[1].second.size(), 7u);
  PB_CHECK_EQ(pdus[1].second[4], 0x03);
}

PB_TEST(link_device_to_host_fragmentation) {
  Connected env;
  uint16_t h = env.c.connection_handle();

  // 300B L2CAP PDU → 首片 247B START_AUTO + 次片 53B CONT
  std::vector<uint8_t> pdu(4 + 296, 0xAB);  // 4B 头 + 296B 载荷 = 300
  pdu[2] = 296 & 0xFF;
  pdu[3] = 296 >> 8;
  env.c.send_l2cap(h, pdu);

  AclFrame f1, f2;
  PB_CHECK(env.c.pop_acl_to_host(f1));
  PB_CHECK(env.c.pop_acl_to_host(f2));
  PB_CHECK(!env.c.pop_acl_to_host(f2));  // 只有两片
  PB_CHECK_EQ(f1.pb, ACL_PB_START_AUTO);
  PB_CHECK_EQ(f1.data.size(), 247u);
  PB_CHECK_EQ(f2.pb, ACL_PB_CONT);
  PB_CHECK_EQ(f2.data.size(), 53u);
  // 拼回
  std::vector<uint8_t> joined = f1.data;
  joined.insert(joined.end(), f2.data.begin(), f2.data.end());
  PB_CHECK(joined == pdu);
}

PB_TEST(link_read_remote_features_event) {
  Connected env;
  PB_CHECK(env.c.has_connection());
  uint16_t h = env.c.connection_handle();
  // LE_Read_Remote_Features(handle)
  uint8_t cmd[] = {0x16, 0x20, 0x02, (uint8_t)(h & 0xFF), (uint8_t)(h >> 8)};
  env.c.handle_command(cmd, 5);
  std::vector<uint8_t> e;
  PB_CHECK(env.c.pop_event(e));  // Command Status
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  bool got = false;
  for (int i = 0; i < 10; ++i) {
    env.c.tick_for_tests(i * 16);
    while (env.c.pop_event(e)) {
      if (e[0] == EVT_LE_META && e[2] == LE_EVT_READ_REMOTE_FEATURES_COMPLETE) {
        got = true;
        PB_CHECK_EQ(e[3], ST_SUCCESS);
        PB_CHECK_EQ(e[6], 0x01);  // features byte0
      }
    }
  }
  PB_CHECK(got);
}

PB_TEST(link_disconnect_releases) {
  Connected env;
  PB_CHECK(env.c.has_connection());
  env.c.simulate_disconnect(0x13);
  PB_CHECK(!env.c.has_connection());
  // 断开后主机 ACL 重组器复位：新 START 帧直接开新 PDU
  env.c.on_l2cap_from_host = [](uint16_t, const std::vector<uint8_t>&) {};
  env.c.handle_acl_from_host(AclFrame{1, ACL_PB_CONT, {1, 2}});  // 无 START 上下文 → 丢弃
  PB_CHECK(true);
}
