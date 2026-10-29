#include "test/test_framework.h"
#include "control/commands.h"

#include <algorithm>

using namespace pb;

PB_TEST(commands_load_list_inject) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));  // 随机端口，避免与其他测试冲突
  auto r1 = control::execute(sim, "load-hid profiles/vendor.json");
  PB_CHECK_EQ(r1[0].substr(0, 3), std::string("OK "));
  auto busid = r1[0].substr(4);
  PB_CHECK_EQ(busid, std::string("7-1"));

  auto r2 = control::execute(sim, "list");
  PB_CHECK_EQ(r2.back(), std::string("OK-END"));
  PB_CHECK(std::any_of(r2.begin(), r2.end(), [&](auto& s) {
    return s.find(busid) != std::string::npos && s.find("1209:B00D") != std::string::npos;
  }));

  auto r3 = control::execute(sim, "inject " + busid + " DEADBEEF");
  PB_CHECK_EQ(r3[0], std::string("OK injected 4 bytes"));

  auto r4 = control::execute(sim, "inject 9-9 00");
  PB_CHECK_EQ(r4[0].substr(0, 3), std::string("ERR"));

  auto r5 = control::execute(sim, "bogus-cmd");
  PB_CHECK_EQ(r5[0].substr(0, 3), std::string("ERR"));

  auto r6 = control::execute(sim, "inject " + busid + " zz");
  PB_CHECK_EQ(r6[0].substr(0, 3), std::string("ERR"));  // 坏 hex

  auto r7 = control::execute(sim, "");
  PB_CHECK_EQ(r7[0].substr(0, 3), std::string("ERR"));

  auto r8 = control::execute(sim, "help");
  PB_CHECK(r8[0].find("load-hid") != std::string::npos);

  auto r9 = control::execute(sim, "status");
  PB_CHECK(r9[0].find("usbip-port=") == 0);
  sim.stop();
}

PB_TEST(commands_load_missing_profile_errors) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  auto r = control::execute(sim, "load-hid profiles/nope.json");
  PB_CHECK_EQ(r[0].substr(0, 3), std::string("ERR"));
  PB_CHECK(r[0].find("nope.json") != std::string::npos);
  sim.stop();
}

PB_TEST(commands_load_two_devices_seq_busid) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  auto a = control::execute(sim, "load-hid profiles/vendor.json");
  auto b = control::execute(sim, "load-hid profiles/gamepad.json");
  PB_CHECK_EQ(a[0], std::string("OK 7-1"));
  PB_CHECK_EQ(b[0], std::string("OK 7-2"));
  sim.stop();
}
