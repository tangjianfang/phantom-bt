#include "test/test_framework.h"
// 覆盖补测：src/hci/link.cpp / src/bluetooth/adapter_model.cpp / src/hci/controller.cpp
// 未覆盖行（OpenCppCoverage R1）。全部纯软件、确定性：无线程、无时序、无硬件。
#include <utility>
#include <vector>

#include "bluetooth/adapter_model.h"
#include "hci/controller.h"
#include "hci/link.h"
#include "hci/packets.h"

using namespace pb;
using namespace pb::hci;

namespace {
// 匿名命名空间：避免与其他测试 TU 的同名结构体 ODR 合并
struct CovBtHciSink final : pb::usb::IUsbModelSink {
  int completes = 0;
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> data;
  void complete_in(uint8_t ep, int32_t, std::vector<uint8_t> d) override {
    ++completes;
    data.emplace_back(ep, std::move(d));
  }
  void stall_ep(uint8_t) override {}
};
}  // namespace

// ———————— src/hci/link.cpp：LinkTable 查找/释放（16-25 行） ————————

PB_TEST(cov_bt_hci_link_table_find_by_peer_release) {
  LinkTable t;
  PB_CHECK_EQ(t.size(), 0u);
  uint16_t h = t.alloc(0xAABBCCDDEE11ull, 1);
  PB_CHECK_EQ(h, 0x0001);
  PB_CHECK_EQ(t.size(), 1u);

  Link* l = t.find(h);
  PB_CHECK(l != nullptr);
  PB_CHECK_EQ(l->handle, h);
  PB_CHECK_EQ(l->peer_addr, 0xAABBCCDDEE11ull);
  PB_CHECK_EQ(l->peer_addr_type, 1);
  PB_CHECK(l->connected);
  PB_CHECK(t.find(0x0BAD) == nullptr);  // 未知句柄

  Link* p = t.find_by_peer(0xAABBCCDDEE11ull);
  PB_CHECK(p != nullptr);
  PB_CHECK_EQ(p->handle, h);
  PB_CHECK(t.find_by_peer(0xDEADFULL) == nullptr);  // 未知对端

  t.release(h);
  PB_CHECK_EQ(t.size(), 0u);
  PB_CHECK(t.find(h) == nullptr);
  PB_CHECK(t.find_by_peer(0xAABBCCDDEE11ull) == nullptr);  // 释放后查不到
}

// ———————— src/hci/link.cpp：post_to_host/take_out（29-40 行） ————————

PB_TEST(cov_bt_hci_link_post_to_host_take_out) {
  LinkTable t;
  uint16_t h = t.alloc(1, 0);

  // 300B L2CAP PDU → 247B START_AUTO + 53B CONT
  std::vector<uint8_t> pdu(300, 0x5A);
  t.post_to_host(h, pdu);
  auto out = t.take_out();
  PB_CHECK_EQ(out.size(), 2u);
  PB_CHECK_EQ(out[0].handle, h);
  PB_CHECK_EQ(out[0].pb, ACL_PB_START_AUTO);
  PB_CHECK_EQ(out[0].data.size(), 247u);
  PB_CHECK_EQ(out[1].handle, h);
  PB_CHECK_EQ(out[1].pb, ACL_PB_CONT);
  PB_CHECK_EQ(out[1].data.size(), 53u);
  std::vector<uint8_t> joined = out[0].data;
  joined.insert(joined.end(), out[1].data.begin(), out[1].data.end());
  PB_CHECK(joined == pdu);

  // 取走后清空：再次 take_out 无残留
  PB_CHECK_EQ(t.take_out().size(), 0u);

  // 空 PDU：不产生任何帧
  t.post_to_host(h, {});
  PB_CHECK_EQ(t.take_out().size(), 0u);
}

// ———————— src/hci/link.cpp：AclReassembler 短 START 帧复位（45-46 行） ————————

PB_TEST(cov_bt_hci_acl_reassembler_short_start) {
  AclReassembler r;
  // START 帧不足 4B L2CAP 头 → 复位并丢弃
  auto o1 = r.feed(AclFrame{1, ACL_PB_START, {0x01}});
  PB_CHECK(!o1.has_value());
  // 复位后无进行中 PDU：CONT 帧不累积、不产出
  auto o2 = r.feed(AclFrame{1, ACL_PB_CONT, {0x02, 0x00, 0x05, 0x00, 0x09}});
  PB_CHECK(!o2.has_value());
  // 复位生效：新的完整 START 帧直接产出完整 PDU
  auto o3 = r.feed(AclFrame{1, ACL_PB_START, {0x02, 0x00, 0x04, 0x00, 0x0A, 0x0B}});
  PB_CHECK(o3.has_value());
  PB_CHECK_EQ(o3->size(), 6u);
  PB_CHECK_EQ((*o3)[4], 0x0A);
}

// ———————— src/hci/controller.cpp:315：完整 PDU 但未挂 on_l2cap_from_host ————————

PB_TEST(cov_bt_hci_ctrl_l2cap_pdu_without_handler) {
  MiniController c(ControllerConfig{});
  // 未挂接处理器：完整 PDU 只记日志，不入队、不崩溃
  c.handle_acl_from_host(AclFrame{5, ACL_PB_START, {0x02, 0x00, 0x04, 0x00, 0x11, 0x22}});
  PB_CHECK_EQ(c.event_count(), 0u);
  PB_CHECK_EQ(c.op_log().size(), 0u);
  // 对照：挂接后同形状帧正常交付（证明上面确实产出了完整 PDU）
  int delivered = 0;
  c.on_l2cap_from_host = [&](uint16_t handle, const std::vector<uint8_t>& pdu) {
    ++delivered;
    PB_CHECK_EQ(handle, 5u);
    PB_CHECK_EQ(pdu.size(), 6u);
  };
  c.handle_acl_from_host(AclFrame{5, ACL_PB_START, {0x02, 0x00, 0x04, 0x00, 0x11, 0x22}});
  PB_CHECK_EQ(delivered, 1);
}

// ———————— src/hci/controller.cpp:399：Read_Local_Extended_Features page=0 ————————

PB_TEST(cov_bt_hci_ctrl_extended_features_page_zero) {
  MiniController c(ControllerConfig{});
  uint8_t cmd[] = {0x04, 0x10, 0x02, 0x00, 0x00};  // page=0
  c.handle_command(cmd, sizeof(cmd));
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
  PB_CHECK_EQ(e[1], 14u);  // nump+op+status+page+max+8B features
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK_EQ(e[6], 0x00);  // 回显 page
  PB_CHECK_EQ(e[7], 0x01);  // max page
  PB_CHECK_EQ(e[8], 0xFF);  // BR features byte0
  PB_CHECK_EQ(e[12], 0xDB); // byte4 bit6 = LE supported
}

// ———————— src/hci/controller.cpp：单字节返回的信息类读命令 ————————

PB_TEST(cov_bt_hci_ctrl_info_single_byte_reads) {
  MiniController c(ControllerConfig{});
  std::vector<uint8_t> e;
  auto cmd = [&](std::vector<uint8_t> b) -> std::vector<uint8_t>& {
    c.handle_command(b.data(), b.size());
    PB_CHECK(c.pop_event(e));
    PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
    PB_CHECK_EQ(e[5], ST_SUCCESS);
    return e;
  };

  cmd({0x07, 0x10, 0x00});  // Read_Country_Code
  PB_CHECK_EQ(e[1], 5u);
  PB_CHECK_EQ(e[6], 0x00);

  cmd({0x1F, 0x0C, 0x00});  // Read_Authentication_Enable
  PB_CHECK_EQ(e[1], 5u);
  PB_CHECK_EQ(e[6], 0x00);

  cmd({0x44, 0x0C, 0x00});  // Read_Inquiry_Mode
  PB_CHECK_EQ(e[1], 5u);
  PB_CHECK_EQ(e[6], 0x00);

  cmd({0x5A, 0x0C, 0x00});  // Read_Default_Erroneous_Data_Reporting
  PB_CHECK_EQ(e[1], 5u);
  PB_CHECK_EQ(e[6], 0x00);

  cmd({0x66, 0x0C, 0x00});  // Read_Flow_Control_Mode
  PB_CHECK_EQ(e[1], 5u);
  PB_CHECK_EQ(e[6], 0x00);

  PB_CHECK_EQ(c.event_count(), 5u);
  PB_CHECK_EQ(c.unknown_opcodes_logged(), 0);
}

// ———————— src/hci/controller.cpp：带值返回的读命令 ————————

PB_TEST(cov_bt_hci_ctrl_local_value_reads) {
  MiniController c(ControllerConfig{});
  std::vector<uint8_t> e;
  auto cmd = [&](std::vector<uint8_t> b) -> std::vector<uint8_t>& {
    c.handle_command(b.data(), b.size());
    PB_CHECK(c.pop_event(e));
    PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
    PB_CHECK_EQ(e[5], ST_SUCCESS);
    return e;
  };

  cmd({0x25, 0x0C, 0x00});  // Read_Voice_Setting
  PB_CHECK_EQ(e[1], 6u);
  PB_CHECK_EQ(e[6], 0x60);
  PB_CHECK_EQ(e[7], 0x00);

  cmd({0x38, 0x0C, 0x00});  // Read_Num_Broadcast_Retransmissions
  PB_CHECK_EQ(e[6], 0x0A);

  cmd({0x23, 0x0C, 0x00});  // Read_Class_Of_Device
  PB_CHECK_EQ(e[1], 7u);
  PB_CHECK_EQ(e[6], 0x00);
  PB_CHECK_EQ(e[8], 0x00);

  cmd({0x17, 0x0C, 0x00});  // Read_Page_Timeout
  PB_CHECK_EQ(e[6], 0x20);
  PB_CHECK_EQ(e[7], 0x00);

  cmd({0x1C, 0x20, 0x00});  // LE_Read_Supported_States
  PB_CHECK_EQ(e[1], 12u);   // nump+op+status+8B
  PB_CHECK_EQ(e[6], 0x05);  // bit0 可广播+可扫描, bit2 可发起连接
  PB_CHECK_EQ(e[13], 0x00);

  cmd({0x07, 0x20, 0x00});  // LE_Read_Advertising_Tx_Power
  PB_CHECK_EQ(e[1], 6u);
  PB_CHECK_EQ(e[6], 0x00);
  PB_CHECK_EQ(e[7], 0x00);
}

// ———————— src/hci/controller.cpp:541-543：LE_Create_Connection_Cancel ————————

PB_TEST(cov_bt_hci_ctrl_create_connection_cancel) {
  MiniController c(ControllerConfig{});
  // LE_Create_Connection：plen=25 须等于实际参数字节数（parse_command 校验
  // p[2]==n-3）：6(前导) + 6(对端地址) + 13(own_addr_type/时序参数) = 25
  std::vector<uint8_t> create = {0x0D, 0x20, 25, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
  for (int i = 0; i < 6; ++i) create.push_back((uint8_t)((0xC0DE00000001ull >> (8 * i)) & 0xFF));
  for (int i = 0; i < 13; ++i) create.push_back(0);
  c.handle_command(create.data(), create.size());
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[4], 0x0D);  // opcode 回显

  // 幂等取消（无连接时也允许）
  uint8_t cancel[] = {0x0E, 0x20, 0x00};
  c.handle_command(cancel, sizeof(cancel));
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[4], 0x0E);

  // 取消后 tick 投递规范要求的收尾事件（Vol4 PartE 7.8.14）：
  // LE Connection Complete，status=0x42 Operation Cancelled，且不得再有建链完成
  for (int i = 0; i < 30; ++i) c.tick_for_tests((uint64_t)(i * 16));
  int cancel_meta = 0;
  while (c.pop_event(e)) {
    if (e[0] == EVT_LE_META) {
      ++cancel_meta;
      PB_CHECK_EQ(e[2], LE_EVT_CONNECTION_COMPLETE);
      PB_CHECK_EQ(e[3], ST_OPERATION_CANCELLED);
    } else {
      PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);  // 其余只允许残留 CS
    }
  }
  PB_CHECK_EQ(cancel_meta, 1);
  PB_CHECK(!c.has_connection());
  PB_CHECK_EQ(c.event_count(), 3u);
}

// ———————— src/hci/controller.cpp:552-555：LE_Read_Remote_Features 未知句柄 ————————

PB_TEST(cov_bt_hci_ctrl_remote_features_unknown_handle) {
  MiniController c(ControllerConfig{});
  uint8_t cmd[] = {0x16, 0x20, 0x02, 0xE9, 0x03};  // handle = 0x03E9（无此连接）
  c.handle_command(cmd, sizeof(cmd));
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  bool got = false;
  for (int i = 0; i < 8; ++i) {  // 延迟事件计数 3 → 第 4 个 tick 触发
    c.tick_for_tests((uint64_t)(i * 16));
    while (c.pop_event(e)) {
      if (e[0] == EVT_LE_META && e[2] == LE_EVT_READ_REMOTE_FEATURES_COMPLETE) {
        got = true;
        PB_CHECK_EQ(e[3], ST_UNKNOWN_CONN_ID);
        PB_CHECK_EQ(e[4], 0xE9);  // 回显句柄
        PB_CHECK_EQ(e[5], 0x03);
      }
    }
  }
  PB_CHECK(got);
  PB_CHECK(!c.has_connection());
}

// ———————— src/bluetooth/adapter_model.cpp：BULK IN(0x82) + on_ready_to_host 回填 ————————

PB_TEST(cov_bt_hci_adapter_acl_in_and_ready_to_host) {
  CovBtHciSink sink;
  pb::bt::AdapterModel m(pb::hci::ControllerConfig{}, 7, 2);
  m.attach_sink(&sink);
  PB_CHECK_EQ(m.acl_tx_frames(), 0);

  // BULK IN 轮询但控制器无数据 → 挂起
  m.on_in(0x82);
  PB_CHECK_EQ(sink.completes, 0);

  // 设备侧主动发 L2CAP（非主机命令触发）→ on_ready_to_host → 回填挂起的 BULK IN
  std::vector<uint8_t> pdu = {0x02, 0x00, 0x04, 0x00, 0x01, 0x02};
  m.controller().send_l2cap(1, pdu);
  PB_CHECK_EQ(sink.completes, 1);
  PB_CHECK_EQ(sink.data[0].first, 0x82);
  PB_CHECK_EQ(m.acl_tx_frames(), 1);
  auto& b = sink.data[0].second;  // ACL：handle=1 PB=START_AUTO(0x2000) len=6
  PB_CHECK_EQ(b.size(), 10u);
  PB_CHECK_EQ(b[0], 0x01);
  PB_CHECK_EQ(b[1], 0x20);
  PB_CHECK_EQ(b[2], 6);
  PB_CHECK_EQ(b[3], 0x00);
  PB_CHECK_EQ(b[4], 0x02);

  // 队列已有帧时直接轮询：立即完成
  m.controller().send_l2cap(1, pdu);
  m.on_in(0x82);
  PB_CHECK_EQ(sink.completes, 2);
  PB_CHECK_EQ(m.acl_tx_frames(), 2);
  PB_CHECK_EQ(sink.data[1].second.size(), 10u);

  // INT IN 挂起后由设备侧事件（push_event）回填
  m.on_in(0x81);
  PB_CHECK_EQ(sink.completes, 2);
  m.controller().push_event({0x0E, 0x03, 0x01, 0x00, 0x00});
  PB_CHECK_EQ(sink.completes, 3);
  PB_CHECK_EQ(sink.data[2].first, 0x81);
  PB_CHECK_EQ(m.evt_tx_count(), 1);

  // 未知端点轮询：不命中任何分支，直接返回
  m.on_in(0x7F);
  PB_CHECK_EQ(sink.completes, 3);
}

// ———————— bug iad-doc（HS 化实验后反转）：适配器配置描述符不含 IAD，贴合已验证形态 ————————

PB_TEST(bug_iad_doc_bt_adapter_config_no_iad_hs_form) {
  // 设计 R3'（IAD 双接口聚合）已撤销：本机实测 usbhub 未插 usbccgp（BTHUSB 以设备级
  // E0/01/01 兼容 ID 绑定整机），同构项目 DeckBtService 的已验证形态亦无 IAD；
  // 且 HS 化（bcdUSB 2.00 + bulk 512 + speed=3）为 usbip-win2 UDE 根集线器校验前提。
  // 配置描述符应为：9(config) + [9(if0)+7×3(EP)] + 9(if1) = 48（无 8B IAD 块）
  pb::bt::AdapterModel m(pb::hci::ControllerConfig{}, 7, 2);
  const uint8_t get_cfg[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x00};  // GET_DESCRIPTOR config
  auto cfg = m.on_control(pb::usb::parse_setup(get_cfg), {});
  PB_CHECK_EQ(cfg.size(), 48u);
  PB_CHECK_EQ(cfg[2], 48u);   // wTotalLength L
  PB_CHECK_EQ(cfg[3], 0u);    // wTotalLength H
  PB_CHECK_EQ(cfg[4], 2u);    // bNumInterfaces
  // if0 直接位于 config 之后（@9），无 IAD 前缀：bLength=9 / type=4 / number=0 / 3 端点 / E0/01/01
  PB_CHECK_EQ(cfg[9], 9u);
  PB_CHECK_EQ(cfg[10], 4u);   // DT_INTERFACE
  PB_CHECK_EQ(cfg[11], 0u);   // if0 number
  PB_CHECK_EQ(cfg[13], 3u);   // if0 bNumEndpoints
  PB_CHECK_EQ(cfg[14], 0xE0u);
  // if1（@39）不在任何 IAD 内
  PB_CHECK_EQ(cfg[39], 9u);   // if1 bLength
  PB_CHECK_EQ(cfg[40], 4u);
  PB_CHECK_EQ(cfg[41], 1u);   // if1 number
  PB_CHECK_EQ(cfg[43], 0u);   // if1 bNumEndpoints=0（空 SCO alt0）
}

// ———————— R2 追加：src/hci/controller.cpp:560-588 Disconnect / LE_Connection_Update ————————

// 测试辅助：注册可连接广播源 → LE_Create_Connection → tick 越过建链延迟
// （kConnDelayTicks=13，第 14 个 tick 完成，随链 Remote Features 在第 18 个），
// 排空全部事件后返回连接句柄（失败时 PB_CHECK 记失败并返回 0）
static uint16_t cov_bt_hci_establish_le_link(pb::TestCtx& pb_t, MiniController& c, uint64_t peer) {
  c.air().add(std::make_shared<VirtualAdvertiser>(peer, "cov-peer"));
  // plen=25 须等于实际参数字节数（parse_command 校验 p[2]==n-3）
  std::vector<uint8_t> create = {0x0D, 0x20, 25, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
  for (int i = 0; i < 6; ++i) create.push_back((uint8_t)((peer >> (8 * i)) & 0xFF));
  for (int i = 0; i < 13; ++i) create.push_back(0);
  c.handle_command(create.data(), create.size());

  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));  // Command Status(pending)
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);

  uint16_t handle = 0;
  for (int i = 0; i < 20; ++i) {
    c.tick_for_tests((uint64_t)(i * 16));
    while (c.pop_event(e)) {
      if (e[0] == EVT_LE_META && e[2] == LE_EVT_CONNECTION_COMPLETE && e[3] == ST_SUCCESS)
        handle = (uint16_t)(e[4] | (e[5] << 8));
    }
  }
  PB_CHECK(handle != 0);
  PB_CHECK_EQ(handle, c.connection_handle());
  PB_CHECK(!c.pop_event(e));  // 事件已排空（含随链 Remote Features Complete）
  return handle;
}

// ———————— controller.cpp:561-577：Disconnect 已建链句柄 ————————
// Vol4 PartE 7.7.5：CS 起始 + Disconnection_Complete 收尾 + 句柄/链路释放

PB_TEST(cov_bt_hci_ctrl_disconnect_known_handle) {
  MiniController c(ControllerConfig{});
  uint16_t h = cov_bt_hci_establish_le_link(pb_t, c, 0xC0DE00000001ull);
  PB_CHECK_EQ(h, 0x0001);

  // Disconnect(handle=0x0001, reason=0x13 Remote User Terminated)：0x0406, plen=3
  uint8_t disc[] = {0x06, 0x04, 0x03, 0x01, 0x00, 0x13};
  c.handle_command(disc, sizeof(disc));
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);  // 以 CS 起始
  PB_CHECK_EQ(e[4], 0x06);                // opcode 回显 0x0406
  PB_CHECK_EQ(e[5], 0x04);

  int disc_complete = 0;
  for (int i = 0; i < 5; ++i) {  // 延迟计数 2 → 第 3 个 tick 到期
    c.tick_for_tests((uint64_t)(i * 16));
    while (c.pop_event(e)) {
      PB_CHECK_EQ(e[0], EVT_DISCONNECTION_COMPLETE);
      ++disc_complete;
      PB_CHECK_EQ(e[1], 4u);      // status+handle(2)+reason
      PB_CHECK_EQ(e[2], ST_SUCCESS);
      PB_CHECK_EQ(e[3], 0x01);   // 句柄回显
      PB_CHECK_EQ(e[4], 0x00);
      PB_CHECK_EQ(e[5], 0x13);   // reason 回显
    }
  }
  PB_CHECK_EQ(disc_complete, 1);
  PB_CHECK(!c.has_connection());  // 句柄已释放
  PB_CHECK_EQ(c.connection_handle(), 0u);

  // 回归（controller.cpp:564-565 注释）：句柄不释放会让后续建链被静默吞掉
  // （状态永久 wedged）→ 释放后再次建链必须能完成，next_handle_ 递增为 0x0002
  uint16_t h2 = cov_bt_hci_establish_le_link(pb_t, c, 0xC0DE00000001ull);
  PB_CHECK_EQ(h2, 0x0002);
}

// ———————— controller.cpp:573-577：Disconnect 未知句柄 ————————
// 无匹配连接：仍须收尾（status=Unknown Connection Identifier），且不动现有连接

PB_TEST(cov_bt_hci_ctrl_disconnect_unknown_handle) {
  MiniController c(ControllerConfig{});
  uint16_t h = cov_bt_hci_establish_le_link(pb_t, c, 0xC0FFEE000000ull);
  PB_CHECK_EQ(h, 0x0001);

  // Disconnect(handle=0x0BAD)：与现有连接句柄不匹配
  uint8_t disc[] = {0x06, 0x04, 0x03, 0xAD, 0x0B, 0x13};
  c.handle_command(disc, sizeof(disc));
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);

  int disc_complete = 0;
  for (int i = 0; i < 5; ++i) {
    c.tick_for_tests((uint64_t)(i * 16));
    while (c.pop_event(e)) {
      PB_CHECK_EQ(e[0], EVT_DISCONNECTION_COMPLETE);
      ++disc_complete;
      PB_CHECK_EQ(e[2], ST_UNKNOWN_CONN_ID);  // 未知连接标识收尾
      PB_CHECK_EQ(e[3], 0xAD);                // 句柄回显
      PB_CHECK_EQ(e[4], 0x0B);
      PB_CHECK_EQ(e[5], 0x00);                // reason 固定 0（无实际断链）
    }
  }
  PB_CHECK_EQ(disc_complete, 1);
  // 错误句柄的断开不得影响现有连接
  PB_CHECK(c.has_connection());
  PB_CHECK_EQ(c.connection_handle(), h);
  PB_CHECK_EQ(c.connection_peer(), 0xC0FFEE000000ull);
}

// ———————— controller.cpp:578-587：LE_Connection_Update 已建链句柄 ————————
// Vol4 PartE 7.8.18：CS 后以 LE Connection Update Complete 收尾（沿用建链参数）

PB_TEST(cov_bt_hci_ctrl_connection_update_complete) {
  MiniController c(ControllerConfig{});
  uint16_t h = cov_bt_hci_establish_le_link(pb_t, c, 0xC0AB00000042ull);
  PB_CHECK_EQ(h, 0x0001);

  // LE_Connection_Update(0x2013)：handle(2)+min(2)+max(2)+latency(2)+timeout(2)=10B 参数
  std::vector<uint8_t> upd = {0x13, 0x20, 10, 0x01, 0x00, 0x18, 0x00, 0x28, 0x00,
                              0x00, 0x00, 0x48, 0x00};
  c.handle_command(upd.data(), upd.size());
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[4], 0x13);  // opcode 回显 0x2013
  PB_CHECK_EQ(e[5], 0x20);

  int upd_complete = 0;
  for (int i = 0; i < 5; ++i) {  // 延迟计数 2 → 第 3 个 tick 到期
    c.tick_for_tests((uint64_t)(i * 16));
    while (c.pop_event(e)) {
      PB_CHECK_EQ(e[0], EVT_LE_META);
      PB_CHECK_EQ(e[2], LE_EVT_CONNECTION_UPDATE_COMPLETE);
      ++upd_complete;
      PB_CHECK_EQ(e[1], 10u);      // subevent+status+handle(2)+interval(2)+latency(2)+timeout(2)
      PB_CHECK_EQ(e[3], ST_SUCCESS);
      PB_CHECK_EQ(e[4], 0x01);    // 句柄回显
      PB_CHECK_EQ(e[5], 0x00);
      PB_CHECK_EQ(e[6], 0x18);    // interval 30ms（沿用建链值）
      PB_CHECK_EQ(e[7], 0x00);
      PB_CHECK_EQ(e[8], 0x00);    // latency 0
      PB_CHECK_EQ(e[9], 0x00);
      PB_CHECK_EQ(e[10], 0x48);   // timeout 720ms
      PB_CHECK_EQ(e[11], 0x00);
    }
  }
  PB_CHECK_EQ(upd_complete, 1);
  PB_CHECK(c.has_connection());  // 参数更新不断链
  PB_CHECK_EQ(c.connection_handle(), h);
}

// ———————— controller.cpp:560/578/588 守卫：参数不足不进收尾分支 ————————
// Disconnect qn<3、LE_Connection_Update qn<10 → 仅回 CS，永不产生延迟收尾事件

PB_TEST(cov_bt_hci_ctrl_disconnect_update_short_params) {
  MiniController c(ControllerConfig{});
  std::vector<uint8_t> e;

  // Disconnect 只带 2B 参数（qn=2 < 3）
  uint8_t disc_short[] = {0x06, 0x04, 0x02, 0x01, 0x00};
  c.handle_command(disc_short, sizeof(disc_short));
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[4], 0x06);
  PB_CHECK_EQ(e[5], 0x04);
  for (int i = 0; i < 6; ++i) c.tick_for_tests((uint64_t)(i * 16));
  PB_CHECK(!c.pop_event(e));  // 无 Disconnection_Complete 收尾
  PB_CHECK(!c.has_connection());

  // LE_Connection_Update 只带 9B 参数（qn=9 < 10）
  uint8_t upd_short[] = {0x13, 0x20, 9, 0x01, 0x00, 0x18, 0x00,
                         0x28, 0x00, 0x00, 0x00, 0x48};
  c.handle_command(upd_short, sizeof(upd_short));
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[4], 0x13);
  PB_CHECK_EQ(e[5], 0x20);
  for (int i = 0; i < 6; ++i) c.tick_for_tests((uint64_t)(i * 16));
  PB_CHECK(!c.pop_event(e));  // 无 Connection_Update_Complete 收尾
  PB_CHECK(!c.has_connection());
  PB_CHECK_EQ(c.event_count(), 2u);  // 全程仅两次 CS
}

// ===================== 缺陷修复锚定（bug_lebuf / bug_nocp） =====================

// —— bug_lebuf：LE_Read_Buffer_Size 返回参数多拼了一个 0 字节 ——
// Vol4 PartE 7.8.2（Core 5.4）：返回参数仅 Status + LE_ACL_Data_Packet_Length(2,
// LE) + Total_Num_LE_ACL_Data_Packets(1)，并没有独立的 SCO/ISO mtu 字节。原实现
// 多写一个 0（plen=8），主机按规范 7 字节解析时尾字节被截掉/错位。正确布局：
// 0E 07 01 02 20 00 FB 00 08。
PB_TEST(bug_lebuf_le_read_buffer_size_return_params) {
  MiniController c(ControllerConfig{});
  uint8_t cmd[] = {0x02, 0x20, 0x00};  // LE_Read_Buffer_Size
  c.handle_command(cmd, sizeof(cmd));
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
  PB_CHECK_EQ(e.size(), 9u);    // evt(1)+plen(1)+nump(1)+op(2)+status(1)+mtu(2)+num(1)
  PB_CHECK_EQ(e[1], 7u);        // nump+op+status+3B 返回参数
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK_EQ(e[6], 0xFB);      // le_acl_mtu=251 低字节
  PB_CHECK_EQ(e[7], 0x00);      // 高字节
  PB_CHECK_EQ(e[8], 8u);        // le_num_packets=8（最后一个字节即包数）
}

// —— bug_nocp：控制器从不产出 Number_Of_Completed_Packets（ACL credit 不归还） ——
// Vol4 PartE 7.7.19：主机每个 ACL 数据包占用一个控制器缓冲，控制器发送完成后
// 须以 Number_Of_Completed_Packets 事件按在途包数归还 credit，否则主机侧
// credit 用尽后数据面停摆。锚定：包在途期间（tick 前）不产出；tick 完成发送后
// 按句柄聚合产出，且数量 = 该句柄在途包数；无在途包的 tick 不产出。
PB_TEST(bug_nocp_number_of_completed_packets_by_inflight) {
  MiniController c(ControllerConfig{});
  AclFrame f{0x0001, ACL_PB_START, {0x02, 0x00, 0x04, 0x00, 0x11, 0x22}};
  c.handle_acl_from_host(f);
  c.handle_acl_from_host(f);
  std::vector<uint8_t> e;
  PB_CHECK(!c.pop_event(e));  // 在途未完成：无任何事件（含无 NOCP）

  c.tick_for_tests(16);       // 发送完成 → 归还 2 个 credit
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_NUMBER_COMPLETED_PKTS);
  PB_CHECK_EQ(e.size(), 7u);  // evt(1)+plen(1)+num_handles(1)+handle(2)+num(2)
  PB_CHECK_EQ(e[1], 5u);
  PB_CHECK_EQ(e[2], 1u);      // Num_Handles
  PB_CHECK_EQ(e[3], 0x01);    // handle 0x0001 LE
  PB_CHECK_EQ(e[4], 0x00);
  PB_CHECK_EQ(e[5], 2u);      // 完成数 = 在途包数
  PB_CHECK_EQ(e[6], 0u);

  c.tick_for_tests(32);       // 无新增在途包：不再产出
  PB_CHECK(!c.pop_event(e));

  c.handle_acl_from_host(f);  // 再发 1 包 → 下次 tick 只归还 1
  c.tick_for_tests(48);
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_NUMBER_COMPLETED_PKTS);
  PB_CHECK_EQ(e[5], 1u);
  PB_CHECK_EQ(e[6], 0u);
}

// ===================== 缺陷修复锚定（bug_hci_disc） =====================
// 缺陷：主机 HCI Disconnect 只回 Command Status，既不产出 Disconnection_Complete
//（Vol4 PartE 7.7.5 要求 CS 起始、Disconnection_Complete 收尾），也不释放连接
// 句柄/链路 —— conn_handle_ 永久占用，后续 LE_Create_Connection 在
// “conn_handle_==0 才受理”守卫处被静默吞掉，主机状态 wedged。
// 锚定：CS（opcode 回显）→ 延迟 Disconnection_Complete(status=SUCCESS、句柄与
// reason 回显) → 句柄/对端清零 → 再次建链可完成（链路表重新分配，句柄 0x0002）。

PB_TEST(bug_hci_disc_disconnect_completes_and_releases_link) {
  MiniController c(ControllerConfig{});
  uint16_t h = cov_bt_hci_establish_le_link(pb_t, c, 0x0B0D00000001ull);
  PB_CHECK_EQ(h, 0x0001);

  // Disconnect(handle=0x0001, reason=0x13)：opcode 0x0406、plen=3
  uint8_t disc[] = {0x06, 0x04, 0x03, 0x01, 0x00, 0x13};
  c.handle_command(disc, sizeof(disc));

  // 先 Command Status（pending），opcode 回显 0x0406
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[4], 0x06);
  PB_CHECK_EQ(e[5], 0x04);
  // CS 之后、tick 之前：句柄仍在（断链由 Disconnection_Complete 收尾，不得提前释放）
  PB_CHECK(c.has_connection());

  // tick 越过延迟计数 → 恰好一条 Disconnection_Complete
  int disc_complete = 0;
  for (int i = 0; i < 5; ++i) {
    c.tick_for_tests((uint64_t)(i * 16));
    while (c.pop_event(e)) {
      PB_CHECK_EQ(e[0], EVT_DISCONNECTION_COMPLETE);
      ++disc_complete;
      PB_CHECK_EQ(e[1], 4u);      // status+handle(2)+reason
      PB_CHECK_EQ(e[2], ST_SUCCESS);
      PB_CHECK_EQ(e[3], 0x01);   // 句柄回显
      PB_CHECK_EQ(e[4], 0x00);
      PB_CHECK_EQ(e[5], 0x13);   // reason 回显（Remote User Terminated）
    }
  }
  PB_CHECK_EQ(disc_complete, 1);

  // 句柄/对端已释放：消除 wedged（否则后续建链被静默吞掉）
  PB_CHECK(!c.has_connection());
  PB_CHECK_EQ(c.connection_handle(), 0u);
  PB_CHECK_EQ(c.connection_peer(), 0u);

  // 再次建链必须完成：句柄递增为 0x0002
  uint16_t h2 = cov_bt_hci_establish_le_link(pb_t, c, 0x0B0D00000001ull);
  PB_CHECK_EQ(h2, 0x0002);
}
