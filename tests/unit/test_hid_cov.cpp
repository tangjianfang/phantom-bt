#include "test/test_framework.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"
#include "hid/report_descriptor.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pb;
using namespace pb::hid;

namespace {  // 匿名命名空间：避免与其他 TU 的同名结构体 ODR 合并
struct CovSink final : pb::usb::IUsbModelSink {
  int completes = 0, stalls = 0;
  void complete_in(uint8_t, int32_t, std::vector<uint8_t>) override { ++completes; }
  void stall_ep(uint8_t) override { ++stalls; }
};

// 8 字节控制 setup 包 → ControlSetup
static std::array<uint8_t, 8> cov_setup(uint8_t bm, uint8_t br, uint16_t wv, uint16_t wi, uint16_t wl) {
  return {bm, br, (uint8_t)(wv & 0xFF), (uint8_t)(wv >> 8), (uint8_t)(wi & 0xFF), (uint8_t)(wi >> 8),
          (uint8_t)(wl & 0xFF), (uint8_t)(wl >> 8)};
}

static std::unique_ptr<HidModel> cov_make_model(CovSink* sink, const char* json_profile) {
  auto prof = HidProfile::from_json(json::parse(json_profile));
  auto m = std::make_unique<HidModel>(prof, 9, 3);
  if (sink) m->attach_sink(sink);
  return m;
}
}  // namespace

// hid_model.cpp:13 —— report_descriptor_override 存在时构造函数采用覆盖描述符
PB_TEST(cov_hid_model_uses_report_descriptor_override) {
  auto m = cov_make_model(nullptr,
                           "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\","
                           "\"report_descriptor_hex\":\"05010906A101\"}");
  const auto& rd = m->report_descriptor();
  PB_CHECK_EQ(rd.size(), 6u);
  const uint8_t want[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01};
  bool match = rd.size() == 6;
  for (size_t i = 0; match && i < 6; ++i) match = (rd[i] == want[i]);
  PB_CHECK(match);
}

// hid_model.cpp:17 —— mouse 模板构造
PB_TEST(cov_hid_model_mouse_template) {
  auto m = cov_make_model(nullptr,
                           "{\"template\":\"mouse\",\"vid\":\"0x1209\",\"pid\":\"0xB005\",\"product\":\"ms\"}");
  PB_CHECK(m->report_descriptor() == template_mouse());
  PB_CHECK(!m->report_descriptor().empty());
  PB_CHECK_EQ(m->exported().interfaces[0].iprotocol, 2);  // boot mouse 协议
}

// hid_model.cpp:93-97 —— GET_REPORT（类请求 IN）：无历史输入报告 → 8 字节全零
PB_TEST(cov_hid_get_report_empty_returns_8_zeros) {
  CovSink sink;
  auto m = cov_make_model(&sink,
                           "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\"}");
  auto r = m->on_control(pb::usb::parse_setup(cov_setup(0xA1, 0x01, 0x0100, 0, 8).data()), {});
  PB_CHECK_EQ(r.size(), 8u);
  bool all_zero = true;
  for (uint8_t b : r) all_zero = all_zero && (b == 0);
  PB_CHECK(all_zero);
}

// hid_model.cpp:94/96/97 —— GET_REPORT 返回最近一次输入报告，并按 wLength 截断
PB_TEST(cov_hid_get_report_returns_last_report_and_truncates) {
  CovSink sink;
  auto m = cov_make_model(&sink,
                           "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\"}");
  // 注入并经一次 IN 轮询送达 → 记录为 last_input_report_
  m->inject_input_report({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
  m->on_in(0x81);
  PB_CHECK_EQ(sink.completes, 1);

  auto full = m->on_control(pb::usb::parse_setup(cov_setup(0xA1, 0x01, 0x0100, 0, 0xFF).data()), {});
  PB_CHECK_EQ(full.size(), 8u);
  PB_CHECK_EQ(full[0], 0x11);
  PB_CHECK_EQ(full[7], 0x88);

  auto trunc = m->on_control(pb::usb::parse_setup(cov_setup(0xA1, 0x01, 0x0100, 0, 3).data()), {});
  PB_CHECK_EQ(trunc.size(), 3u);
  PB_CHECK_EQ(trunc[0], 0x11);
  PB_CHECK_EQ(trunc[2], 0x33);
}

// hid_model.cpp:93（OUT 方向分支）—— GET_REPORT 方向错误 → STALL
PB_TEST(cov_hid_get_report_out_direction_stalls) {
  CovSink sink;
  auto m = cov_make_model(&sink,
                           "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\"}");
  PB_CHECK_THROWS(m->on_control(pb::usb::parse_setup(cov_setup(0x21, 0x01, 0x0100, 0, 0).data()), {}));
  PB_CHECK(m->stalled_recently);
}

// hid_model.cpp:167 —— on_stall_clear 复位 stalled_recently
PB_TEST(cov_hid_stall_clear_resets_flag) {
  CovSink sink;
  auto m = cov_make_model(&sink,
                           "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\"}");
  PB_CHECK_THROWS(m->on_control(pb::usb::parse_setup(cov_setup(0xA1, 0x7F, 0, 0, 0).data()), {}));
  PB_CHECK(m->stalled_recently);
  m->on_stall_clear(0x81);
  PB_CHECK(!m->stalled_recently);
}

// profiles.cpp:41 —— 缺少 pid 字段抛错
PB_TEST(cov_hid_profile_missing_pid_throws) {
  PB_CHECK_THROWS(HidProfile::from_json(json::parse("{\"template\":\"vendor\",\"vid\":\"0x1209\"}")));
}
