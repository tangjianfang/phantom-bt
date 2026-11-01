#include "test/test_framework.h"
#include "ble/att.h"
#include "ble/device_stack.h"
#include "ble/gatt_db.h"
#include "ble/l2cap.h"
#include "ble/services.h"
#include "ble/smp.h"
#include "hci/controller.h"
#include "hid/report_descriptor.h"
#include "hci/packets.h"

using namespace pb;
using namespace pb::hci;
using namespace pb::ble;

// ===== FakeHost：模拟 Windows 主机栈（直连 MiniController 接口）=====
namespace {

struct E2E {
  MiniController controller{ControllerConfig{}};
  std::unique_ptr<DeviceStack> stack;
  uint16_t h = 0;

  E2E() {
    controller.air().add(std::make_shared<VirtualAdvertiser>(0xC01122334455ull, "PhantomKb"));
    stack = std::make_unique<DeviceStack>(controller);
    stack->setup_keyboard("PhantomKb");
  }

  void drain_events() {
    std::vector<uint8_t> e;
    while (controller.pop_event(e)) {
    }
  }

  bool connect() {
    std::vector<uint8_t> cmd = {0x0D, 0x20, 24, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; ++i) cmd.push_back((uint8_t)((0xC01122334455ull >> (8 * i)) & 0xFF));
    for (int i = 0; i < 12; ++i) cmd.push_back(0);
    controller.handle_command(cmd.data(), cmd.size());
    drain_events();
    for (int i = 0; i < 30; ++i) controller.tick_for_tests(i * 16);
    std::vector<uint8_t> e;
    bool ok = false;
    while (controller.pop_event(e)) {
      if (e[0] == EVT_LE_META && e[2] == LE_EVT_CONNECTION_COMPLETE && e[3] == ST_SUCCESS) ok = true;
    }
    h = controller.connection_handle();
    return ok;
  }

  // 主机发送 ATT PDU（经 ACL START 帧），取回应答（含 L2CAP 头剥离）
  std::optional<std::vector<uint8_t>> att_request(const std::vector<uint8_t>& att_pdu) {
    controller.handle_acl_from_host(AclFrame{h, ACL_PB_START, build_l2cap(L2CAP_CID_ATT, att_pdu)});
    // 设备应答会同步入队（on_l2cap_from_host → send_l2cap）
    AclFrame f;
    if (!controller.pop_acl_to_host(f)) return std::nullopt;
    // 剥 ACL 帧头后即完整 L2CAP PDU（247 上限内不分片）
    uint16_t cid;
    std::vector<uint8_t> payload;
    if (!parse_l2cap(f.data, cid, payload)) return std::nullopt;
    return payload;
  }

  // 收取设备主动通知（等价于中断驱动的 InputReportReceived）
  std::optional<std::vector<uint8_t>> take_notification() {
    AclFrame f;
    if (!controller.pop_acl_to_host(f)) return std::nullopt;
    uint16_t cid;
    std::vector<uint8_t> payload;
    if (!parse_l2cap(f.data, cid, payload)) return std::nullopt;
    return payload;
  }
};

}  // namespace

PB_TEST(e2e_ble_keyboard_full_chain) {
  E2E t;
  PB_CHECK(t.connect());
  PB_CHECK_EQ(t.h, 0x0001);

  // 1) MTU Exchange
  auto mtu = t.att_request({ATT_MTU_REQ, 0x0B, 0x02});  // 523
  PB_CHECK(mtu.has_value());
  PB_CHECK_EQ((*mtu)[0], ATT_MTU_RSP);
  PB_CHECK_EQ((uint16_t)((*mtu)[1] | ((*mtu)[2] << 8)), 247u);

  // 2) 服务发现
  auto svcs = t.att_request({ATT_READ_BY_GROUP_REQ, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28});
  PB_CHECK(svcs.has_value());
  PB_CHECK_EQ((*svcs)[0], ATT_READ_BY_GROUP_RSP);
  PB_CHECK_EQ(svcs->size(), 20u);  // 3 服务

  // 3) HID 特征发现（HID Info）
  auto info = t.att_request({ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0xFF, 0xFF, 0x4A, 0x2A});
  PB_CHECK(info.has_value());
  PB_CHECK_EQ((*info)[0], ATT_READ_BY_TYPE_RSP);

  // 4) 读 Report Map（MTU 523 协商后有效 247 → 完整返回）
  auto map = t.att_request({ATT_READ_REQ, H_REPORT_MAP & 0xFF, H_REPORT_MAP >> 8});
  PB_CHECK(map.has_value());
  PB_CHECK_EQ((*map)[0], ATT_READ_RSP);
  PB_CHECK_EQ(map->size() - 1, hid::template_ble_keyboard_reportmap().size());

  // 5) 写 Protocol Mode = report
  auto pm = t.att_request({ATT_WRITE_REQ, H_PROTOCOL_MODE & 0xFF, H_PROTOCOL_MODE >> 8, 0x01});
  PB_CHECK(pm.has_value());
  PB_CHECK_EQ((*pm)[0], ATT_WRITE_RSP);
  PB_CHECK_EQ(t.stack->hogp().protocol_mode(), 0x01);

  // 6) 发现 CCC 并订阅
  auto fi = t.att_request({ATT_FIND_INFO_REQ, 0x11, 0x00, 0x19, 0x00});
  PB_CHECK(fi.has_value());
  PB_CHECK_EQ((*fi)[0], ATT_FIND_INFO_RSP);
  auto wr = t.att_request({ATT_WRITE_REQ, H_REPORT1_CCC & 0xFF, H_REPORT1_CCC >> 8, 0x01, 0x00});
  PB_CHECK(wr.has_value());
  PB_CHECK_EQ((*wr)[0], ATT_WRITE_RSP);

  // 7) 输入报告通知：注入 'h' 键 → ATT 通知
  std::vector<uint8_t> report = {0x02, 0x00, 'h', 0, 0, 0, 0, 0};  // mod=LeftShift? 0x02=LeftShift
  PB_CHECK(t.stack->hogp().send_input_report(report));
  auto ntf = t.take_notification();
  PB_CHECK(ntf.has_value());
  PB_CHECK_EQ((*ntf)[0], ATT_HANDLE_VALUE_NTF);
  PB_CHECK_EQ((uint16_t)((*ntf)[1] | ((*ntf)[2] << 8)), H_REPORT1_VALUE);
  PB_CHECK_EQ((*ntf)[3], 0x02);
  PB_CHECK_EQ((*ntf)[5], 'h');

  // 8) 断开
  t.controller.simulate_disconnect(0x13);
  std::vector<uint8_t> e;
  PB_CHECK(t.controller.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_DISCONNECTION_COMPLETE);
  PB_CHECK(!t.controller.has_connection());
}

PB_TEST(e2e_ble_keyboard_typing_sequence) {
  E2E t;
  PB_CHECK(t.connect());
  // 订阅
  t.att_request({ATT_MTU_REQ, 0x0B, 0x02});
  auto wr = t.att_request({ATT_WRITE_REQ, H_REPORT1_CCC & 0xFF, H_REPORT1_CCC >> 8, 0x01, 0x00});
  PB_CHECK(wr.has_value());

  // 连续键入 "hi"：按下+抬起
  const char* text = "hi";
  int notifications = 0;
  for (const char* p = text; *p; ++p) {
    PB_CHECK(t.stack->hogp().send_input_report({0x00, 0x00, (uint8_t)*p, 0, 0, 0, 0, 0}));
    auto n1 = t.take_notification();
    PB_CHECK(n1.has_value());
    PB_CHECK_EQ((*n1)[5], *p);
    ++notifications;
    PB_CHECK(t.stack->hogp().send_input_report({0, 0, 0, 0, 0, 0, 0, 0}));  // 抬起
    auto n2 = t.take_notification();
    PB_CHECK(n2.has_value());
    ++notifications;
  }
  PB_CHECK_EQ(notifications, 4);
}

PB_TEST(e2e_smp_just_works_over_l2cap) {
  E2E t;
  PB_CHECK(t.connect());
  // SMP 走 CID 0x0006
  auto step = [&](const std::vector<uint8_t>& pdu) -> std::optional<std::vector<uint8_t>> {
    t.controller.handle_acl_from_host(AclFrame{t.h, ACL_PB_START, build_l2cap(L2CAP_CID_SM, pdu)});
    AclFrame f;
    if (!t.controller.pop_acl_to_host(f)) return std::nullopt;
    uint16_t cid;
    std::vector<uint8_t> payload;
    if (!parse_l2cap(f.data, cid, payload)) return std::nullopt;
    return payload;
  };
  auto r1 = step({0x01, 0x03, 0x00, 0x00, 0x10, 0x03, 0x00});
  PB_CHECK(r1.has_value());
  PB_CHECK_EQ((*r1)[0], 0x02);
  auto r2 = step([&] {
    std::vector<uint8_t> p{0x03};
    for (int i = 0; i < 16; ++i) p.push_back((uint8_t)(0xA0 + i));
    return p;
  }());
  PB_CHECK(r2.has_value());
  PB_CHECK_EQ((*r2)[0], 0x03);
  auto r3 = step([&] {
    std::vector<uint8_t> p{0x04};
    for (int i = 0; i < 16; ++i) p.push_back((uint8_t)(0x50 + i));
    return p;
  }());
  PB_CHECK(r3.has_value());
  PB_CHECK_EQ((*r3)[0], 0x04);  // 高优#6：Pairing Random 应答须回显 0x04（0x05=Pairing Failed）
  PB_CHECK(t.stack->smp().paired());
}

PB_TEST(e2e_unsubscribed_report_dropped) {
  E2E t;
  PB_CHECK(t.connect());
  PB_CHECK(!t.stack->hogp().send_input_report({0, 0, 'x', 0, 0, 0, 0, 0}));
  PB_CHECK_EQ(t.stack->hogp().dropped_reports(), 1);
  AclFrame f;
  PB_CHECK(!t.controller.pop_acl_to_host(f));
}
