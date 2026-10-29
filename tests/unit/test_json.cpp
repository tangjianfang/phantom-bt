#include "test/test_framework.h"
#include "core/json.h"

PB_TEST(json_object_nested_array) {
  auto v = pb::json::parse(R"({"template":"gamepad","vid":"0x1234","axes":[8,16],"flags":true})");
  PB_CHECK_EQ(v.find("template")->as_str(), std::string("gamepad"));
  PB_CHECK_EQ(v.find("axes")->arr.size(), 2u);
  PB_CHECK_EQ(v.find("axes")->arr[0].as_num(), 8.0);
  PB_CHECK_EQ(v.find("axes")->arr[1].as_num(), 16.0);
  PB_CHECK(v.find("flags")->as_bool());
  PB_CHECK(v.find("missing") == nullptr);
}

PB_TEST(json_rejects_garbage) {
  PB_CHECK_THROWS(pb::json::parse("{ nope"));
  PB_CHECK_THROWS(pb::json::parse(""));
  PB_CHECK_THROWS(pb::json::parse("[1,2"));
  PB_CHECK_THROWS(pb::json::parse("\"abc"));
  PB_CHECK_THROWS(pb::json::parse("{\"a\":}"));
}

PB_TEST(json_escapes) {
  auto v = pb::json::parse("{\"s\":\"a\\\"b\\\\c\\n\\u4e2d\"}");
  PB_CHECK_EQ(v.find("s")->as_str(), std::string("a\"b\\c\n\xe4\xb8\xad"));
}

PB_TEST(json_numbers_and_nested_objects) {
  auto v = pb::json::parse(R"({"a":-1.5,"b":{"c":[[1,2],[3]]},"n":null})");
  PB_CHECK_EQ(v.find("a")->as_num(), -1.5);
  PB_CHECK_EQ(v.find("b")->find("c")->arr[1].arr[0].as_num(), 3.0);
  PB_CHECK_EQ(v.find("n")->type, pb::json::Value::Null);
}

PB_TEST(json_empty_containers) {
  auto v = pb::json::parse(R"({"o":{},"a":[]})");
  PB_CHECK_EQ(v.find("o")->obj.size(), 0u);
  PB_CHECK_EQ(v.find("a")->arr.size(), 0u);
}
