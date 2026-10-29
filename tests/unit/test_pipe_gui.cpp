#include "test/test_framework.h"
#include "control/pipe_server.h"

#include <windows.h>

#include "control/commands.h"

using namespace pb;

PB_TEST(pipe_roundtrip_status) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  control::PipeServer pipe(sim);
  PB_CHECK(pipe.start(&err));

  std::vector<std::string> replies;
  bool ok = control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt", "status", replies, 3000);
  PB_CHECK(ok);
  PB_CHECK(!replies.empty());
  PB_CHECK(replies[0].rfind("OK usbip-port=", 0) == 0);

  pipe.stop();
  sim.stop();
}

PB_TEST(pipe_roundtrip_load_and_list) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  control::PipeServer pipe(sim);
  PB_CHECK(pipe.start(&err));

  std::vector<std::string> r1;
  PB_CHECK(control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt", "load-hid profiles/vendor.json", r1));
  PB_CHECK_EQ(r1[0], std::string("OK 7-1"));

  std::vector<std::string> r2;
  PB_CHECK(control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt", "list", r2));
  PB_CHECK_EQ(r2.back(), std::string("OK-END"));

  std::vector<std::string> r3;
  PB_CHECK(control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt", "bogus", r3));
  PB_CHECK(r3[0].rfind("ERR", 0) == 0);

  pipe.stop();
  sim.stop();
}

PB_TEST(pipe_client_rejects_missing_pipe) {
  std::vector<std::string> replies;
  PB_CHECK(!control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt-nope", "status", replies));
}

// GUI 冒烟：窗口类注册/创建/销毁不崩溃（无消息循环，不进入交互）
PB_TEST(gui_smoke_window_class_registerable) {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "PhantomBTGuiSmoke";
  ATOM atom = RegisterClassA(&wc);
  PB_CHECK(atom != 0);
  HWND w = CreateWindowExA(0, "PhantomBTGuiSmoke", "smoke", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
  PB_CHECK(w != nullptr);
  ShowWindow(w, SW_HIDE);
  PB_CHECK(DestroyWindow(w));
  PB_CHECK(UnregisterClassA("PhantomBTGuiSmoke", wc.hInstance));
}

PB_TEST(ble_type_requires_connection) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  auto r = control::execute(sim, "load-bt");
  PB_CHECK_EQ(r[0], std::string("OK 7-1"));
  auto r2 = control::execute(sim, "ble-type C0:11:22:33:44:55 hello");
  PB_CHECK_EQ(r2[0].substr(0, 3), std::string("ERR"));  // 未连接
  sim.stop();
}
