#include "test/test_framework.h"
#include "core/env_check.h"
#include "core/net.h"

using namespace pb;

PB_TEST(env_os_build_threshold) {
  PB_CHECK(pb::env::os_build_ok(18362));   // Win10 1903
  PB_CHECK(pb::env::os_build_ok(26100));   // Win11 24H2
  PB_CHECK(!pb::env::os_build_ok(18361));  // 1903 之前
  PB_CHECK(!pb::env::os_build_ok(17763));  // 1809
}

PB_TEST(env_classify_os) {
  PB_CHECK_EQ(pb::env::classify_os(26100, 10), std::string("Windows 11 (build 26100)"));
  PB_CHECK_EQ(pb::env::classify_os(19045, 10), std::string("Windows 10 (build 19045)"));
  // Pre-Win10 keeps its real minor version (e.g. Win7 SP1 = 6.1 build 7601).
  PB_CHECK_EQ(pb::env::classify_os(7601, 6, 1), std::string("Windows 6.1 (build 7601)"));
}

PB_TEST(env_foreign_vid_enum_key) {
  // Our own virtual adapter never counts.
  PB_CHECK(!pb::env::foreign_vid_enum_key("VID_0B00&PID_0001", "VID_0B00"));
  // Any other well-formed VID&PID key is a candidate for the class probe
  // (BT classification itself is by device class, not by VID whitelist).
  PB_CHECK(pb::env::foreign_vid_enum_key("VID_0BDA&PID_8153", "VID_0B00"));   // Realtek NIC key
  PB_CHECK(pb::env::foreign_vid_enum_key("VID_8087&PID_0026", "VID_0B00"));   // Intel BT key
  // Malformed / non-device keys rejected.
  PB_CHECK(!pb::env::foreign_vid_enum_key("VID_1209&PID_B007", "VID_1209"));  // our HID VID
  PB_CHECK(!pb::env::foreign_vid_enum_key("ROOT\\usb", "VID_0B00"));
  PB_CHECK(!pb::env::foreign_vid_enum_key("", "VID_0B00"));
  PB_CHECK(!pb::env::foreign_vid_enum_key("VID_8087", "VID_0B00"));  // no &PID part
}

// Class-E0 detection keys on the Bluetooth subclass, never on vendor.
PB_TEST(env_class_e0_compatid) {
  auto mksz = [](std::vector<std::string> parts) {
    std::string s;
    for (auto& p : parts) s += p + std::string(1, '\0');
    return s += '\0';  // double-NUL terminator
  };
  PB_CHECK(pb::env::has_class_e0_compatid(
      mksz({"USB\\Class_E0&SubClass_01&Prot_01", "USB\\DEV"}).c_str()));
  PB_CHECK(pb::env::has_class_e0_compatid(
      mksz({"USB\\Class_e0&Subclass_01&Prot_01"}).c_str()));  // case-insensitive
  // SubClass_02 is UWB / Certified Wireless USB — not Bluetooth.
  PB_CHECK(!pb::env::has_class_e0_compatid(
      mksz({"USB\\Class_E0&SubClass_02"}).c_str()));
  PB_CHECK(!pb::env::has_class_e0_compatid(
      mksz({"USB\\Class_E0"}).c_str()));  // bare class without SubClass_01
  PB_CHECK(!pb::env::has_class_e0_compatid(
      mksz({"USB\\Class_09&Prot_01", "USB\\VID_0BDA&PID_8153"}).c_str()));  // hub / NIC
  PB_CHECK(!pb::env::has_class_e0_compatid(mksz({}).c_str()));
  PB_CHECK(!pb::env::has_class_e0_compatid(nullptr));
}

PB_TEST(env_urls_not_empty) {
  PB_CHECK(pb::env::usbip_download_url().rfind("https://", 0) == 0);
  PB_CHECK(pb::env::usbip_download_url().find("usbip-win2") != std::string::npos);
}

// —— 真机检测在当前环境可跑且自洽 ——
PB_TEST(env_run_checks_shape) {
  auto checks = pb::env::run_checks();
  PB_CHECK_EQ(checks.size(), 7u);
  bool has_os = false, has_usbip = false, has_admin = false;
  for (auto& c : checks) {
    PB_CHECK(!c.title.empty());
    PB_CHECK(!c.id.empty());
    if (c.id == "os") {
      has_os = true;
      // Status must agree with the machine's actual build (the suite may run
      // on pre-1903 VMs where the Fail branch is the correct answer).
      bool ok_expected = pb::env::os_build_ok(pb::env::windows_build());
      PB_CHECK_EQ((int)c.status, (int)(ok_expected ? pb::env::Status::Ok
                                                   : pb::env::Status::Fail));
    }
    if (c.id == "usbip_exe") has_usbip = true;
    if (c.id == "admin") has_admin = true;
  }
  PB_CHECK(has_os && has_usbip && has_admin);
}

PB_TEST(env_needs_attention_logic) {
  std::vector<pb::env::CheckResult> all_ok(2);
  all_ok[0].status = pb::env::Status::Ok;
  all_ok[1].status = pb::env::Status::Ok;
  PB_CHECK(!pb::env::needs_attention(all_ok));
  all_ok[1].status = pb::env::Status::Warn;
  PB_CHECK(pb::env::needs_attention(all_ok));
}

// Port self-exclusion: a listener owned by this process must not count as
// "someone else's" port. (After the GUI's embedded server binds 3240, a connect()
// probe would flag our own process as a second running instance.)
PB_TEST(env_port_excludes_self) {
  net::WsaSession wsa;
  net::TcpListener l;
  std::string err;
  PB_CHECK(l.listen(0, &err));  // 0 = let the OS assign a free port
  uint16_t port = l.port();
  PB_CHECK(port != 0);
  // Our own listener -> not owned by another process
  PB_CHECK(!pb::env::port_listened_by_other(port));
  l.close();
  // Closed -> no listener entry for this process either
  PB_CHECK(!pb::env::port_listened_by_other(port));
}
