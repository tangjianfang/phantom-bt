#include "test/test_framework.h"
#include "bluetooth/adapter_model.h"

#include <array>
#include <cstdio>

#include "core/bytes.h"

using namespace pb;
using namespace pb::bt;

namespace {  // 匿名命名空间：防止与其他测试 TU 的同名结构体 ODR 合并（inline 成员被 COMDAT 折叠）
struct FakeSink final : pb::usb::IUsbModelSink {
  int completes = 0;
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> data;
  void complete_in(uint8_t ep, int32_t, std::vector<uint8_t> d) override {
    ++completes;
    data.emplace_back(ep, std::move(d));
  }
  void stall_ep(uint8_t) override {}
};
}  // namespace

static std::array<uint8_t, 8> su8(uint8_t bm, uint8_t br, uint16_t wv, uint16_t wi, uint16_t wl) {
  return {bm, br, (uint8_t)(wv & 0xFF), (uint8_t)(wv >> 8), (uint8_t)(wi & 0xFF), (uint8_t)(wi >> 8),
          (uint8_t)(wl & 0xFF), (uint8_t)(wl >> 8)};
}

PB_TEST(bt_adapter_descriptors_match_spec) {
  AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  auto& e = m.exported();
  PB_CHECK_EQ(e.dev_class, 0xE0u);
  PB_CHECK_EQ(e.dev_subclass, 1u);
  PB_CHECK_EQ(e.dev_protocol, 1u);
  PB_CHECK_EQ(e.num_interfaces, 2u);
  PB_CHECK_EQ(e.interfaces.size(), 2u);
  PB_CHECK_EQ(e.interfaces[1].iclass, 0xE0);
  // 控制描述符：设备级 E0 + 配置 2 接口
  auto dev = m.on_control(pb::usb::parse_setup(su8(0x80, 0x06, 0x0100, 0, 18).data()), {});
  PB_CHECK_EQ(dev.size(), 18u);
  PB_CHECK_EQ(dev[4], 0xE0);
  PB_CHECK_EQ(dev[5], 0x01);
  PB_CHECK_EQ(dev[6], 0x01);
  PB_CHECK_EQ(dev[7], 64);
  auto cfg = m.on_control(pb::usb::parse_setup(su8(0x80, 0x06, 0x0200, 0, 255).data()), {});
  // 9 + 9+21 + 9 = 48
  PB_CHECK_EQ(cfg.size(), 48u);
  PB_CHECK_EQ(cfg[4], 2);
  PB_CHECK_EQ(cfg[14], 0xE0);   // if0 class（if0@9, class@9+5）
  PB_CHECK_EQ(cfg[20], 0x81);   // EP1 INT IN（EP1@18, addr@20）
  PB_CHECK_EQ(cfg[21], 3);      // interrupt
  PB_CHECK_EQ(cfg[22], 16);     // max 16
  PB_CHECK_EQ(cfg[27], 0x02);   // EP2 BULK OUT（EP2@25, addr@27）
  PB_CHECK_EQ(cfg[28], 2);
  PB_CHECK_EQ(cfg[34], 0x82);   // EP3 BULK IN（EP3@32, addr@34）
  PB_CHECK_EQ(cfg[35], 2);
  PB_CHECK_EQ(cfg[43], 0);      // if1@39 numEP=0（空 SCO alt0）
  PB_CHECK_EQ(cfg[44], 0xE0);   // if1 class
}

PB_TEST(bt_adapter_hci_over_ep0_and_event_intr) {
  FakeSink sink;
  AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  // EP0 类请求携带 Reset 命令（wLength=3 → data stage 3 字节）
  auto resp = m.on_control(pb::usb::parse_setup(su8(0x20, 0x00, 0, 0, 3).data()), {0x03, 0x0C, 0x00});
  PB_CHECK(resp.empty());
  m.on_in(0x81);  // 取事件
  PB_CHECK_EQ(sink.completes, 1);
  PB_CHECK_EQ(sink.data[0].second[0], pb::hci::EVT_COMMAND_COMPLETE);

  // 先轮询再发命令：命令产生的命令应立即回填挂起轮询
  m.on_in(0x81);
  PB_CHECK_EQ(sink.completes, 1);  // 仍挂起
  m.on_control(pb::usb::parse_setup(su8(0x20, 0x00, 0, 0, 3).data()), {0x09, 0x10, 0x00});
  PB_CHECK_EQ(sink.completes, 2);  // 立即完成
  PB_CHECK_EQ(sink.data[1].second[0], pb::hci::EVT_COMMAND_COMPLETE);
}

PB_TEST(bt_adapter_legacy_e0_request_accepted) {
  FakeSink sink;
  AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  m.on_control(pb::usb::parse_setup(su8(0xE0, 0x00, 0, 0, 3).data()), {0x03, 0x0C, 0x00});
  m.on_in(0x81);
  PB_CHECK_EQ(sink.completes, 1);
}

PB_TEST(bt_adapter_unknown_ep0_stalls) {
  FakeSink sink;
  AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  bool threw = false;
  try {
    m.on_control(pb::usb::parse_setup(su8(0x21, 0x7F, 0, 0, 0).data()), {});
  } catch (pb::usb::ControlStall&) {
    threw = true;
  }
  PB_CHECK(threw);
}

PB_TEST(bt_adapter_full_init_sequence_reaches_ready) {
  FakeSink sink;
  AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  auto send_cmd = [&](std::vector<uint8_t> cmd) {
    m.on_control(pb::usb::parse_setup(su8(0x20, 0x00, 0, 0, (uint16_t)cmd.size()).data()), cmd);
  };
  send_cmd({0x03, 0x0C, 0x00});  // Reset
  send_cmd({0x09, 0x10, 0x00});  // Read_BD_ADDR
  send_cmd({0x01, 0x10, 0x00});  // Read_Local_Version
  send_cmd({0x05, 0x10, 0x00});  // Read_Buffer_Size
  send_cmd({0x02, 0x10, 0x00});  // Read_Local_Supported_Commands
  send_cmd({0x03, 0x10, 0x00});  // Read_Local_Supported_Features
  PB_CHECK(m.controller().radio_ready());
  // 逐事件取回：6 条 Command Complete
  int cc = 0;
  for (int i = 0; i < 6; ++i) {
    m.on_in(0x81);
    if (i < 6) {
      // 每次轮询完成一条
    }
  }
  PB_CHECK_EQ(sink.completes, 6);
  for (auto& [ep, d] : sink.data) PB_CHECK_EQ(d[0], 0x0E);
  PB_CHECK_EQ(m.evt_tx_count(), 6);
}

PB_TEST(bt_adapter_bulk_out_acl_reassembly) {
  FakeSink sink;
  AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  // 完整 ACL 帧：handle=1 PB=START len=4 data 01020304
  std::vector<uint8_t> frame = {0x01, 0x00, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04};
  // 分两个 USB 包：5 字节 + 3 字节
  m.on_out(0x02, {frame.begin(), frame.begin() + 5});
  PB_CHECK_EQ(m.acl_rx_frames(), 0);  // 未完成
  m.on_out(0x02, {frame.begin() + 5, frame.end()});
  PB_CHECK_EQ(m.acl_rx_frames(), 1);
  // 两帧连发
  std::vector<uint8_t> two = frame;
  two.insert(two.end(), frame.begin(), frame.end());
  m.on_out(0x02, two);
  PB_CHECK_EQ(m.acl_rx_frames(), 3);
  // 非端点忽略
  m.on_out(0x01, {0xFF});
  PB_CHECK_EQ(m.acl_rx_frames(), 3);
}
