#include "test/test_framework.h"
#include "hci/le_air.h"
#include "hci/packets.h"

using namespace pb;
using namespace pb::hci;

// 事件布局：0x3E len subevent(0x02) num evt_type addr_type addr[6] len data rssi
PB_TEST(air_adv_report_event_layout) {
  VirtualAir air;
  auto d = std::make_shared<VirtualAdvertiser>(0xC01122334455ull, "PhantomKb");
  d->set_adv_data_name("PhantomKb");
  d->set_scan_response("PhantomKb");
  air.add(d);
  air.set_host_scanning(true, false);  // 被动扫描
  std::vector<std::vector<uint8_t>> events;
  air.tick(1000, [&](std::vector<uint8_t> e) { events.push_back(e); });
  PB_CHECK_EQ(events.size(), 1u);

  auto& e = events[0];
  PB_CHECK_EQ(e[0], EVT_LE_META);
  PB_CHECK_EQ(e[1], 26u);        // sub1+num1+evt1+atype1+addr6+len1+data14+rssi1
  PB_CHECK_EQ(e[2], LE_EVT_ADVERTISING_REPORT);
  PB_CHECK_EQ(e[3], 1u);         // num reports
  PB_CHECK_EQ(e[4], 0x00);       // ADV_IND（connectable）
  PB_CHECK_EQ(e[5], 1u);         // random address
  PB_CHECK_EQ(e[6], 0x55);       // addr LSB first
  PB_CHECK_EQ(e[11], 0xC0);      // addr MSB
  PB_CHECK_EQ(e[12], 14u);       // data_len = flags AD(3) + name AD(11)
  PB_CHECK_EQ(e[13], 0x02);      // flags len
  PB_CHECK_EQ(e[14], 0x01);      // flags type
  PB_CHECK_EQ(e[15], 0x06);      // flags value
  PB_CHECK_EQ(e[16], 10u);       // name AD len（type1 + "PhantomKb"9）
  PB_CHECK_EQ(e[17], 0x09);      // name type
  PB_CHECK_EQ(e[18], 'P');
  int rssi_idx = 13 + 14;
  PB_CHECK_EQ(e[rssi_idx], 0xC0);  // -64dBm
}

PB_TEST(air_active_scan_emits_scan_rsp) {
  VirtualAir air;
  auto d = std::make_shared<VirtualAdvertiser>(0xC0AABBCCDDEEull, "Kb");
  d->set_adv_data_name("Kb");
  d->set_scan_response("Kb");
  air.add(d);
  air.set_host_scanning(true, true);  // 主动扫描
  std::vector<std::vector<uint8_t>> events;
  air.tick(1000, [&](std::vector<uint8_t> e) { events.push_back(e); });
  PB_CHECK_EQ(events.size(), 2u);      // ADV_IND + SCAN_RSP
  PB_CHECK_EQ(events[1][4], 0x04);     // SCAN_RSP evt_type
  PB_CHECK_EQ(events[1][6], 0xEE);     // addr LSB
}

PB_TEST(air_tick_pacing_and_scan_off) {
  VirtualAir air;
  auto d = std::make_shared<VirtualAdvertiser>(0xC00000000001ull, "x");
  d->set_adv_data_name("x");
  air.add(d);
  air.set_host_scanning(true, false);
  int count = 0;
  auto cb = [&](std::vector<uint8_t>) { ++count; };
  air.tick(0, cb);       // 首次
  air.tick(50, cb);      // 距上次 50ms < 128 → 不发
  air.tick(127, cb);
  air.tick(200, cb);     // 200-0=200 ≥128 → 发
  PB_CHECK_EQ(count, 2);
  air.set_host_scanning(false, false);
  air.tick(1000, cb);
  PB_CHECK_EQ(count, 2);
}

PB_TEST(air_ibeacon_payload) {
  auto d = std::make_shared<VirtualAdvertiser>(0xC0AABBCCDDEEull, "beacon", false);
  std::vector<uint8_t> body = {0x02, 0x15};
  for (int i = 0; i < 16; ++i) body.push_back((uint8_t)i);  // UUID
  body.push_back(0x00); body.push_back(0x01);                // major
  body.push_back(0x00); body.push_back(0x02);                // minor
  body.push_back(0xC5);                                       // tx power
  d->set_adv_data_manufacturer(0x004C, body);
  auto& pl = d->payload();
  // 布局：flags AD(02 01 06) + manufacturer AD(len=26, FF, 4C 00, body23)
  PB_CHECK_EQ(pl.adv[0], 0x02u);
  PB_CHECK_EQ(pl.adv[1], 0x01u);
  PB_CHECK_EQ(pl.adv[2], 0x06u);
  PB_CHECK_EQ(pl.adv[3], 26u);   // manufacturer AD len（type1+company2+body23）
  PB_CHECK_EQ(pl.adv[4], 0xFFu);
  PB_CHECK_EQ(pl.adv[5], 0x4C);  // company LE
  PB_CHECK_EQ(pl.adv[6], 0x00);
  PB_CHECK_EQ(pl.adv[7], 0x02);  // iBeacon type
  PB_CHECK_EQ(pl.adv[8], 0x15);  // iBeacon len
  PB_CHECK_EQ(pl.adv_len, 3u + 27u);
}

PB_TEST(air_nonconnectable_report_type) {
  VirtualAir air;
  auto d = std::make_shared<VirtualAdvertiser>(0xC00000000002ull, "bcn", false);
  d->set_adv_data_name("bcn");
  air.add(d);
  air.set_host_scanning(true, false);
  std::vector<std::vector<uint8_t>> events;
  air.tick(0, [&](std::vector<uint8_t> e) { events.push_back(e); });
  PB_CHECK_EQ(events[0][4], 0x02);  // ADV_NONCONN_IND
}

PB_TEST(air_connect_lookup) {
  VirtualAir air;
  auto a = std::make_shared<VirtualAdvertiser>(0xC00000000003ull, "kb");
  auto b = std::make_shared<VirtualAdvertiser>(0xC00000000004ull, "bcn", false);
  air.add(a);
  air.add(b);
  PB_CHECK(air.find_connectable(0xC00000000003ull) != nullptr);
  PB_CHECK(air.find_connectable(0xC00000000004ull) == nullptr);  // 非 connectable
  PB_CHECK(air.find_connectable(0xC00000000009ull) == nullptr);
  PB_CHECK(air.find_any(0xC00000000004ull) != nullptr);
  air.remove(0xC00000000003ull);
  PB_CHECK(air.find_any(0xC00000000003ull) == nullptr);
  PB_CHECK_EQ(air.devices().size(), 1u);
}

PB_TEST(air_adv_data_raw_truncated_to_31) {
  auto d = std::make_shared<VirtualAdvertiser>(0xC00000000005ull, "x");
  std::vector<uint8_t> big(40, 0xAA);
  d->set_adv_data(big);
  PB_CHECK_EQ(d->payload().adv_len, 31u);
}

PB_TEST(air_name_too_long_truncated) {
  auto d = std::make_shared<VirtualAdvertiser>(0xC00000000006ull, "x");
  d->set_adv_data_name(std::string(50, 'N'));
  // flags(3) + name AD 头(2) + ≤26 字符
  PB_CHECK(d->payload().adv_len <= 31u);
  PB_CHECK(d->payload().adv_len >= 3u);
}
