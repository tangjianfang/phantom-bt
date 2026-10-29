#include "test/test_framework.h"
#include "hid/profiles.h"

using namespace pb;
using namespace pb::hid;

PB_TEST(profile_load_gamepad_json) {
  auto p = HidProfile::load_file("profiles/gamepad.json");
  PB_CHECK_EQ(p.template_name, std::string("gamepad"));
  PB_CHECK_EQ(p.vid, 0x1209u);
  PB_CHECK_EQ(p.pid, 0xB007u);
  PB_CHECK_EQ(p.product, std::string("Phantom Gamepad"));
  PB_CHECK_EQ(p.serial, std::string("PG-0001"));
}

PB_TEST(profile_load_all_templates) {
  for (auto& f : {"vendor", "keyboard", "mouse"}) {
    auto p = HidProfile::load_file(std::string("profiles/") + f + ".json");
    PB_CHECK_EQ(p.template_name, std::string(f));
    PB_CHECK_EQ(p.vid, 0x1209u);
  }
}

PB_TEST(profile_rejects_unknown_template) {
  auto v = json::parse("{\"template\":\"printer\",\"vid\":\"0x1\",\"pid\":\"0x2\",\"product\":\"x\"}");
  PB_CHECK_THROWS(HidProfile::from_json(v));
}

PB_TEST(profile_rejects_missing_fields) {
  PB_CHECK_THROWS(HidProfile::from_json(json::parse("{\"vid\":\"0x1\"}")));
  PB_CHECK_THROWS(HidProfile::from_json(json::parse("{\"template\":\"vendor\"}")));
}

PB_TEST(profile_parse_u16) {
  PB_CHECK_EQ(parse_u16("0x1234"), 0x1234u);
  PB_CHECK_EQ(parse_u16("4660"), 0x1234u);
  PB_CHECK_EQ(parse_u16("0xFFFF"), 0xFFFFu);
  PB_CHECK_EQ(parse_u16("0"), 0u);
  PB_CHECK_THROWS(parse_u16("0x10000"));  // 溢出
  PB_CHECK_THROWS(parse_u16("zz"));
  PB_CHECK_THROWS(parse_u16(""));
}

PB_TEST(profile_report_descriptor_override) {
  auto p = HidProfile::from_json(json::parse(
      "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\","
      "\"report_descriptor_hex\":\"05010901A101C0\"}"));
  PB_CHECK(p.report_descriptor_override.has_value());
  PB_CHECK_EQ(p.report_descriptor_override->size(), 7u);
  PB_CHECK_EQ((*p.report_descriptor_override)[0], 0x05);
}

PB_TEST(profile_missing_file) {
  PB_CHECK_THROWS(HidProfile::load_file("profiles/nope.json"));
}
