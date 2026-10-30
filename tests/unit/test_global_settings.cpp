#include "test/test_framework.h"

#include <string>

#include "control/commands.h"
#include "sim/simulator.h"

using namespace pb;

namespace {

// Fresh simulator per test; commands run without the usbip server (pure model
// layer) — attach is never exercised here. Named the_sim(): a helper named
// sim() collides with the pb::sim namespace under `using namespace pb`.
sim::Simulator& the_sim() {
  static sim::Simulator s;  // stopped at process exit
  return s;
}

bool run_ok(const std::string& line) {
  auto r = control::execute(the_sim(), line);
  return !r.empty() && r[0].rfind("OK", 0) == 0;
}

}  // namespace

PB_TEST(gs_unload_unknown_busid) {
  auto r = control::execute(the_sim(), "unload 9-9");
  PB_CHECK(r[0].rfind("ERR", 0) == 0);
  PB_CHECK(r[0].find("unknown busid") != std::string::npos);
}

PB_TEST(gs_unload_all_empty) {
  auto r = control::execute(the_sim(), "unload-all");
  PB_CHECK(r[0].find("unloaded 0 device") != std::string::npos);
}

PB_TEST(gs_ble_clear_without_adapter) {
  auto r = control::execute(the_sim(), "ble-clear");
  PB_CHECK(r[0].rfind("OK", 0) == 0);
  PB_CHECK(r[0].find("cleared 0") != std::string::npos);
}

PB_TEST(gs_load_bt_then_unload_roundtrip) {
  PB_CHECK(run_ok("load-bt"));
  auto before = control::execute(the_sim(), "list");
  size_t devs_before = 0;
  for (auto& l : before)
    if (l.rfind("DEV ", 0) == 0) ++devs_before;
  PB_CHECK(devs_before >= 1);
  auto r = control::execute(the_sim(), "unload-all");
  PB_CHECK(r[0].find("unloaded 1 device") != std::string::npos ||
           r[0].find("unloaded ") != std::string::npos);
  auto after = control::execute(the_sim(), "list");
  for (auto& l : after) PB_CHECK(l.rfind("DEV ", 0) != 0);  // registry drained
}

PB_TEST(gs_ble_advertise_clear_roundtrip) {
  PB_CHECK(run_ok("load-bt"));
  auto adv = control::execute(the_sim(), "ble-advertise profiles/ble_keyboard.json");
  PB_CHECK(adv[0].rfind("OK", 0) == 0);
  auto cleared = control::execute(the_sim(), "ble-clear");
  PB_CHECK(cleared[0].find("cleared 1") != std::string::npos);
  auto listed = control::execute(the_sim(), "ble-list");
  bool empty = true;
  for (auto& l : listed)
    if (l != "OK" && l != "OK-END" && !l.empty()) empty = false;
  PB_CHECK(empty);
  run_ok("unload-all");
}

PB_TEST(gs_sys_bt_list_shape) {
  auto r = control::execute(the_sim(), "sys-bt list");
  PB_CHECK_EQ(r.front(), std::string("OK"));
  PB_CHECK_EQ(r.back(), std::string("OK-END"));
  // Body: either the "none found" line or one [state] line per radio.
  for (size_t i = 1; i + 1 < r.size(); ++i)
    PB_CHECK(r[i].find('[') != std::string::npos || r[i].find("no physical") == 0);
}

PB_TEST(gs_sys_bt_bad_op) {
  auto replies = control::execute(the_sim(), "sys-bt explode");
  PB_CHECK(replies[0].rfind("ERR", 0) == 0);
  PB_CHECK(replies[0].find("usage") != std::string::npos);
}
