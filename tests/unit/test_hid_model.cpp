#include "test/test_framework.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"

#include <array>

#include "core/bytes.h"

using namespace pb;
using namespace pb::hid;

namespace {  // 匿名命名空间：避免与其他 TU 的同名结构体 ODR 合并
struct FakeSink final : pb::usb::IUsbModelSink {
  int completes = 0, stalls = 0;
  std::vector<std::vector<uint8_t>> data;
  void complete_in(uint8_t, int32_t, std::vector<uint8_t> d) override {
    ++completes;
    data.push_back(std::move(d));
  }
  void stall_ep(uint8_t) override { ++stalls; }
};
}  // namespace

static std::unique_ptr<HidModel> make_vendor_model(FakeSink* sink) {
  auto prof = HidProfile::from_json(json::parse(
      "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\",\"product\":\"v\"}"));
  auto m = std::make_unique<HidModel>(prof, 7, 1);
  if (sink) m->attach_sink(sink);
  return m;
}

static std::array<uint8_t, 8> su8(uint8_t bm, uint8_t br, uint16_t wv, uint16_t wi, uint16_t wl) {
  return {bm, br, (uint8_t)(wv & 0xFF), (uint8_t)(wv >> 8), (uint8_t)(wi & 0xFF), (uint8_t)(wi >> 8),
          (uint8_t)(wl & 0xFF), (uint8_t)(wl >> 8)};
}

PB_TEST(hid_exported_metadata) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  auto& e = m->exported();
  PB_CHECK_EQ(e.vid, 0x1209u);
  PB_CHECK_EQ(e.pid, 0xB00Du);
  PB_CHECK_EQ(e.busid, std::string("7-1"));
  PB_CHECK_EQ(e.num_interfaces, 1u);
  PB_CHECK_EQ(e.interfaces[0].iclass, 3);
}

PB_TEST(hid_get_report_descriptor_via_control) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  auto resp = m->on_control(pb::usb::parse_setup(su8(0x81, 0x06, 0x2200, 0, 0xFFFF).data()), {});
  PB_CHECK_EQ(resp.size(), m->report_descriptor().size());
  PB_CHECK_EQ(resp.size(), 27u);  // template_vendor 字节数
}

PB_TEST(hid_standard_descriptors_served) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  auto dev = m->on_control(pb::usb::parse_setup(su8(0x80, 0x06, 0x0100, 0, 18).data()), {});
  PB_CHECK_EQ(dev.size(), 18u);
  auto cfg = m->on_control(pb::usb::parse_setup(su8(0x80, 0x06, 0x0200, 0, 255).data()), {});
  // 9 + 9(iface) + 9(hid) + 7×2(ep) = 41
  PB_CHECK_EQ(cfg.size(), 41u);
  PB_CHECK_EQ(cfg[14], 3);   // iface class HID
  PB_CHECK_EQ(cfg[29], 0x81u);
  PB_CHECK_EQ(cfg[36], 0x01u);
}

PB_TEST(hid_inject_completes_pending) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  m->on_in(0x81);  // Windows 轮询先到 → 挂起
  PB_CHECK_EQ(sink.completes, 0);
  m->inject_input_report({0x01, 0x02, 0x03});
  PB_CHECK_EQ(sink.completes, 1);
  PB_CHECK_EQ(sink.data[0].size(), 3u);

  m->inject_input_report({0x04});       // 无挂起 → 入队
  m->inject_input_report({0x05});       // 再入队
  m->on_in(0x81);                       // 下一次轮询立即取队首
  PB_CHECK_EQ(sink.completes, 2);
  PB_CHECK_EQ(sink.data[1].size(), 1u);
  PB_CHECK_EQ(sink.data[1][0], 0x04);
}

PB_TEST(hid_queue_drop_oldest_when_full) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  for (int i = 0; i < 20; ++i) m->inject_input_report({(uint8_t)i});
  PB_CHECK_EQ(m->dropped_reports(), 4);
  m->on_in(0x81);
  PB_CHECK_EQ(sink.data[0][0], 4);  // 前 4 个被丢
}

PB_TEST(hid_output_report_via_interrupt_out_and_set_report) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  m->on_out(0x01, {0xAA, 0xBB});
  auto r = m->pop_output_report();
  PB_CHECK(r.has_value());
  PB_CHECK_EQ(r->size(), 2u);
  PB_CHECK_EQ((*r)[1], 0xBB);
  PB_CHECK(!m->pop_output_report().has_value());

  m->on_control(pb::usb::parse_setup(su8(0x21, 0x09, 0x0200, 0, 2).data()), {0xCC});
  r = m->pop_output_report();
  PB_CHECK(r.has_value());
  PB_CHECK_EQ((*r)[0], 0xCC);
}

PB_TEST(hid_idle_protocol_class_requests) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  auto idle = m->on_control(pb::usb::parse_setup(su8(0xA1, 0x02, 0, 0, 1).data()), {});
  PB_CHECK_EQ(idle.size(), 1u);
  PB_CHECK_EQ(idle[0], 0);
  m->on_control(pb::usb::parse_setup(su8(0x21, 0x0A, 0x0700, 0, 0).data()), {});
  PB_CHECK_EQ(m->idle(), 7u);
  auto proto = m->on_control(pb::usb::parse_setup(su8(0xA1, 0x03, 0, 0, 1).data()), {});
  PB_CHECK_EQ(proto[0], 1);  // 默认 report 协议
  m->on_control(pb::usb::parse_setup(su8(0x21, 0x0B, 0x0000, 0, 0).data()), {});
  PB_CHECK_EQ(m->protocol(), 0);  // boot
}

PB_TEST(hid_unknown_class_request_stalls) {
  FakeSink sink;
  auto m = make_vendor_model(&sink);
  bool threw = false;
  try {
    m->on_control(pb::usb::parse_setup(su8(0xA1, 0x7F, 0, 0, 0).data()), {});
  } catch (pb::usb::ControlStall&) {
    threw = true;
  }
  PB_CHECK(threw);
  PB_CHECK(m->stalled_recently);
}

PB_TEST(hid_keyboard_profile_uses_keyboard_template) {
  FakeSink sink;
  auto prof = HidProfile::from_json(json::parse(
      "{\"template\":\"keyboard\",\"vid\":\"0x1209\",\"pid\":\"0xB001\",\"product\":\"kb\"}"));
  HidModel m(prof, 7, 2);
  PB_CHECK_EQ(m.exported().busid, std::string("7-2"));
  PB_CHECK_EQ(m.report_descriptor().size() > 27, true);  // 主集合 + vendor
  PB_CHECK_EQ(m.exported().interfaces[0].iprotocol, 1);  // boot keyboard 协议
}
