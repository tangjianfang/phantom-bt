// 入口（src/cli/main.cpp、src/gui/main.cpp）依赖的库级序列覆盖。
// argv 解析/子命令分发/退出码语义是进程级行为，由 tools/check-tools.ps1 派生
// phantombt.exe 断言（src/cli 不在单测覆盖率口径内）；本文件覆盖两个入口 main
// 直接调用的库组合序列，此前只有零散的单命令/单协议测试：
//   1) serve 启动序列（指定端口 → 管道 → load/load-bt/ble 顺序加载 → status）
//   2) `cmd` 管道客户端会话（同一 bench 上 status→load→list→inject→unload→status）
//   3) --probe 注入线程的 65B payload 形状经完整 usbip TCP 回路
//   4) GUI wWinMain 启动序列（固定 3240 + gs_refresh 轮询的命令组合）
#include "test/test_framework.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "cli/serve_args.h"
#include "control/commands.h"
#include "control/pipe_server.h"
#include "core/bytes.h"
#include "core/net.h"
#include "sim/simulator.h"
#include "usbip/protocol.h"

using namespace pb;

namespace {

// usbip 导入真实客户端序列：op_common(0x0111 + OP_REQ_IMPORT) + 32B busid。
// 与 src/cli/main.cpp:160-173（selftest）相同的线上字节（经 Simulator 而非裸 Registry）。
bool import_busid(net::TcpConn& c, const std::string& busid) {
  std::string err;
  ByteWriter w;
  w.u16be(usbip::OP_VERSION);
  w.u16be(usbip::OP_REQ_IMPORT);
  w.u32be(0);
  char bid[32] = {};
  std::strncpy(bid, busid.c_str(), 31);
  w.bytes((const uint8_t*)bid, 32);
  if (!c.send_all(w.buf().data(), w.buf().size(), &err)) return false;
  uint8_t imp[8] = {};
  if (!c.recv_exact(imp, 8, &err)) return false;
  if (imp[7] != 0) return false;  // status != 0 → 拒绝
  std::vector<uint8_t> idev(312);
  return c.recv_exact(idev.data(), idev.size(), &err);
}

}  // namespace

// run_serve（src/cli/main.cpp:35-121，无 --attach/--probe 分支）的库级等价：
// 指定端口起 usbip 服务（--port）→ 控制管道 → --load/--load-bt/--ble 按 serve
// 的固定顺序加载 → status 报 3 设备 → 管道侧看到同样状态 → unload-all 清空。
PB_TEST(entry_serve_library_sequence) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 3941));  // --port 3941（run-m 真机腿同款端口）
  PB_CHECK(sim.port() >= 3941);     // start(want) 从 want 起依次重试
  auto c = net::TcpConn::connect("127.0.0.1", sim.port(), &err);
  PB_CHECK(c.is_open());  // usbip 端口真实监听（serve 启动横幅的来源）
  c.close();

  control::PipeServer pipe(sim);
  PB_CHECK(pipe.start(&err, "\\\\.\\pipe\\phantom-bt-entry-a"));

  // serve 的加载顺序：loads → load-bt → bles（main.cpp:50-76）。
  // ble-advertise 依赖 bt adapter 先就位（simulator.cpp:110 无 adapter 即抛错）。
  PB_CHECK_EQ(sim.cmd_load_hid_profile("profiles/vendor.json"), std::string("7-1"));
  PB_CHECK_EQ(sim.cmd_load_bt_adapter(), std::string("7-2"));
  PB_CHECK(sim.cmd_ble_advertise("profiles/ble_keyboard.json").rfind("OK ", 0) == 0);

  // status = "usbip-port=<N> devices=<M>"（simulator.cpp:371）。devices 只数
  // DeviceRegistry 里的 USB 设备（HID+BT=2）；BLE advertiser 挂在 controller 的
  // air 上，不占注册表位（cmd_ble_advertise 实现，simulator.cpp:102-116）。
  std::string st = sim.cmd_status();
  PB_CHECK(st.find("usbip-port=" + std::to_string(sim.port())) != std::string::npos);
  PB_CHECK(st.find("devices=2") != std::string::npos);

  // 控制管道侧看到同一状态 —— serve 实际暴露给 `phantombt cmd` 的路径
  std::vector<std::string> rep;
  PB_CHECK(control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt-entry-a", "status", rep, 3000));
  if (!rep.empty()) PB_CHECK_EQ(rep[0], std::string("OK ") + st);
  std::vector<std::string> lst;
  PB_CHECK(control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt-entry-a", "list", lst, 3000));
  size_t devs = 0;
  for (auto& l : lst)
    if (l.rfind("DEV ", 0) == 0) ++devs;
  PB_CHECK_EQ(devs, (size_t)2);

  // --ble 的可观测效果：广播中的设备出现在 ble-list（profiles/ble_keyboard.json
  // 地址 C0:11:22:33:44:55）
  std::vector<std::string> bl;
  PB_CHECK(control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt-entry-a", "ble-list", bl, 3000));
  bool adv_seen = false;
  for (auto& l : bl)
    if (l.find("C0:11:22:33:44:55") != std::string::npos) adv_seen = true;
  PB_CHECK(adv_seen);

  PB_CHECK_EQ(sim.cmd_unload_all(), std::string("unloaded 2 device(s)"));
  pipe.stop();
  sim.stop();  // run_serve 的收尾顺序（main.cpp:118-119）
}

// `phantombt cmd`（src/cli/main.cpp:239-248）的库级等价：单个 bench 实例上，
// CommandClient 串行跑完 status → load-hid → list → inject → unload-all → status
// 的完整会话 —— 外部客户端驱动 serve/GUI 内嵌 PipeServer 的实际形态。
// 既有 test_pipe_gui.cpp 只覆盖 status/load/list/bogus 四条离散命令。
PB_TEST(entry_cmd_client_session_roundtrip) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  control::PipeServer pipe(sim);
  PB_CHECK(pipe.start(&err, "\\\\.\\pipe\\phantom-bt-entry-b"));
  const char* P = "\\\\.\\pipe\\phantom-bt-entry-b";
  auto ask = [&](const char* line, std::vector<std::string>& out) {
    out.clear();
    bool ok = control::CommandClient::send_line(P, line, out, 3000);
    PB_CHECK(ok);
    if (out.empty()) PB_CHECK(!out.empty());
    return out;
  };

  std::vector<std::string> r;
  PB_CHECK(ask("status", r)[0].rfind("OK usbip-port=", 0) == 0);
  PB_CHECK_EQ(ask("load-hid profiles/vendor.json", r)[0], std::string("OK 7-1"));

  auto rl = ask("list", r);
  size_t devs = 0;
  for (auto& l : rl)
    if (l.rfind("DEV ", 0) == 0) ++devs;
  PB_CHECK_EQ(devs, (size_t)1);
  if (!rl.empty()) PB_CHECK_EQ(rl.back(), std::string("OK-END"));

  // inject 走 execute 的 from_hex 路径 → cmd_inject 返回 "injected N bytes"
  PB_CHECK_EQ(ask("inject 7-1 c0ffee", r)[0], std::string("OK injected 3 bytes"));

  PB_CHECK(ask("unload-all", r)[0].rfind("OK unloaded 1 device", 0) == 0);
  PB_CHECK(ask("status", r)[0].find("devices=0") != std::string::npos);

  pipe.stop();
  sim.stop();
}

// --probe 周期注入线程（src/cli/main.cpp:95-105）的 payload 形状经完整 usbip
// TCP 回路可用：Report ID 0x02 + 64B 0xCD（共 65B）注入后由挂起的 INT IN URB
// 原样收得。既有 server_interrupt_in_pending_then_inject 只验 1-2 字节报告。
// （注入线程本体在 serve 进程内，此处在库级验证同一调用的线上效果。）
PB_TEST(entry_probe_inject_payload_shape) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  std::string busid = sim.cmd_load_hid_profile("profiles/vendor.json");
  PB_CHECK_EQ(busid, std::string("7-1"));

  auto c = net::TcpConn::connect("127.0.0.1", sim.port(), &err);
  PB_CHECK(c.is_open());
  PB_CHECK(import_busid(c, busid));

  // 挂起 INT IN：128B 缓冲 > 65B 报告（与真实 Windows 栈的轮询 URB 同形态）
  ByteWriter w;
  w.u32be(usbip::USBIP_CMD_SUBMIT);
  w.u32be(2);  // seq
  w.u32be(0x00070001);
  w.u32be(usbip::DIR_IN);
  w.u32be(0x81);  // EP1 IN
  w.u32be(0);
  w.u32be(128);  // transfer buffer
  w.u32be(0);
  w.u32be(0);
  w.u32be(10);  // interval
  uint8_t zero8[8] = {};
  w.bytes(zero8, 8);
  PB_CHECK(c.send_all(w.buf().data(), w.buf().size(), &err));
  Sleep(150);  // 等 URB 入挂起队列（selftest 同款节拍，main.cpp:212）

  std::vector<uint8_t> report{0x02};  // main.cpp:99-101 的注入形状
  report.insert(report.end(), 64, 0xCD);
  PB_CHECK_EQ(sim.cmd_inject(busid, report), std::string("injected 65 bytes"));

  std::vector<uint8_t> ret(usbip::RET_SUBMIT_HDR_SIZE + 65);
  PB_CHECK(c.recv_exact(ret.data(), ret.size(), &err));
  PB_CHECK_EQ(ret[usbip::RET_SUBMIT_HDR_SIZE], 0x02);        // Report ID
  PB_CHECK_EQ(ret[usbip::RET_SUBMIT_HDR_SIZE + 64], 0xCD);   // 载荷末字节
  // 全 64B 载荷逐字节核对（CD × 64）
  for (int i = 1; i <= 64; ++i) PB_CHECK_EQ(ret[usbip::RET_SUBMIT_HDR_SIZE + i], 0xCD);

  c.close();
  sim.stop();
}

// GUI wWinMain（src/gui/main.cpp:786-806）的库级启动序列：固定请求 3240 端口
// 起 Simulator + PipeServer，gs_refresh()（main.cpp:121-154）轮询的命令组合
// （sys-bt list / list / ble-list）与状态栏的 status 全部健康返回。
// 窗口/按钮的进程级点击由 tools/gui-verify.ps1 与 tools/check-tools.ps1 覆盖。
PB_TEST(entry_gui_library_sequence) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 3240));  // GUI 固定请求 3240（main.cpp:799）
  PB_CHECK(sim.port() >= 3240);     // 被占用时依次重试（GUI 现状容忍）
  control::PipeServer pipe(sim);
  PB_CHECK(pipe.start(&err, "\\\\.\\pipe\\phantom-bt-entry-c"));

  // gs_refresh 的命令组合（src/gui/main.cpp:123,138-144,150）
  auto sysbt = control::execute(sim, "sys-bt list");
  PB_CHECK(!sysbt.empty());
  if (!sysbt.empty()) {
    PB_CHECK_EQ(sysbt.front(), std::string("OK"));
    PB_CHECK_EQ(sysbt.back(), std::string("OK-END"));
  }
  auto lst = control::execute(sim, "list");
  PB_CHECK(!lst.empty());
  if (!lst.empty()) PB_CHECK_EQ(lst.back(), std::string("OK-END"));
  auto bl = control::execute(sim, "ble-list");
  PB_CHECK(!bl.empty());
  if (!bl.empty()) PB_CHECK_EQ(bl.front(), std::string("OK"));

  // 状态栏（WM_PAINT，main.cpp:715）读取的 status 形状
  auto st = control::execute(sim, "status");
  PB_CHECK(!st.empty());
  if (!st.empty()) PB_CHECK(st[0].find("usbip-port=") != std::string::npos);

  pipe.stop();
  sim.stop();
}

// serve 多次 --load/--ble 的累积与顺序（第 2 轮补测）。run_serve 按向量顺序
// 加载（src/cli/main.cpp:50-76），解析层若重排/去重会让多 profile 的 serve
// 静默少设备——逐元素断言顺序与内容，并覆盖与其它 flag 交错出现的场景。
// test_cli_cov.cpp 只测过单值透传与错误路径，未测多值累积。
PB_TEST(entry_serve_args_multi_load_accumulation_order) {
  auto a = cli::parse_serve_args({"--load", "p1.json", "--port", "3425", "--load", "p2.json",
                                  "--ble", "b1.json", "--load-bt", "--ble", "b2.json"});
  PB_CHECK(a.ok);
  PB_CHECK_EQ(a.loads.size(), 2u);
  if (a.loads.size() == 2) {
    PB_CHECK_EQ(a.loads[0], std::string("p1.json"));
    PB_CHECK_EQ(a.loads[1], std::string("p2.json"));
  }
  PB_CHECK_EQ(a.bles.size(), 2u);
  if (a.bles.size() == 2) {
    PB_CHECK_EQ(a.bles[0], std::string("b1.json"));
    PB_CHECK_EQ(a.bles[1], std::string("b2.json"));
  }
  PB_CHECK(a.load_bt);
  PB_CHECK_EQ(a.port, 3425);

  // 用法错误停在第一处（serve_args.h 约定：ok=false 时其余字段是部分结果，
  // 调用方只打印 usage 并退出，main.cpp:288-296）——中途缺值绝不静默吞掉
  auto bad = cli::parse_serve_args({"--load", "p1.json", "--load"});
  PB_CHECK(!bad.ok);
  PB_CHECK(bad.error.find("--load requires a value") != std::string::npos);
  // attach/probe/exit-after-ms 组合在尾部仍完整解析（真机腿的实际形态：
  // run-m0.ps1 的 serve --attach --probe "<cmdline>"）
  auto t = cli::parse_serve_args(
      {"--attach", "--probe", "probes --address 5544332211C0", "--exit-after-ms", "5000"});
  PB_CHECK(t.ok);
  PB_CHECK(t.attach);
  PB_CHECK_EQ(t.probe, std::string("probes --address 5544332211C0"));
  PB_CHECK_EQ(t.exit_after_ms, 5000);
}

// serve --port 指向被占用端口时的回退（第 3 轮补测）。Simulator::start 对
// want..want+9 顺序重试（src/sim/simulator.cpp:47-67），run_serve 的横幅
// "usbip server on port %u" 打印的是回退后的实际端口（main.cpp:52）——
// exe 层横幅断言见 tools/check-tools.ps1 的 r3-serve-port-busy-falls-back。
// 此处验证同一序列的库级形态：第二个 Simulator 请求第一个已占的端口，
// 必须落到回退窗口内的另一端口，且两个实例都保持可用。
PB_TEST(entry_serve_port_fallback_when_busy) {
  sim::Simulator a;
  std::string err;
  PB_CHECK(a.start(&err, 0));  // 随机空闲端口
  sim::Simulator b;
  PB_CHECK(b.start(&err, (uint16_t)a.port()));  // serve --port <a.port> 的库级等价
  PB_CHECK(b.port() != a.port());  // 被占端口绝不复用
  // 回退窗口 (want, want+9]（want 本身被 a 占用）
  PB_CHECK(b.port() > a.port() && b.port() <= a.port() + 9);
  // 两个实例都健康：各自 status 报告自己的端口
  PB_CHECK(a.cmd_status().find("usbip-port=" + std::to_string(a.port())) != std::string::npos);
  PB_CHECK(b.cmd_status().find("usbip-port=" + std::to_string(b.port())) != std::string::npos);
  b.stop();
  a.stop();
}

// serve --ble 的前置条件 + GUI 201/202 按钮驱动的命令对（第 3 轮补测）。
// run_serve 按 loads→load-bt→bles 顺序加载（main.cpp:50-84）：--ble 若未先
// --load-bt，cmd_ble_advertise 无适配器即抛错（simulator.cpp:147）→ serve
// 打印 "ble <path>: ..." 并 exit 1（exe 层断言 tools/check-tools.ps1 的
// r3-serve-ble-without-adapter-exit1 / r3-serve-ble-bad-profile-exit1）。
// 适配器在场后，advertise/ble-list/ble-stop 是 GUI「加载 BLE 广播」(201) 与
// 「停止选中」(202) 按钮经 run_and_show 实际发出的同一组命令
// （src/gui/main.cpp:383-402）。
PB_TEST(entry_serve_ble_requires_bt_adapter) {
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  // 无适配器：advertise 必须抛错（serve 据此 exit 1，绝不静默假绿）
  PB_CHECK_THROWS(sim.cmd_ble_advertise("profiles/ble_keyboard.json"));
  // 适配器就位后同一调用成功（--load-bt 在 --ble 之前的顺序依赖）
  PB_CHECK_EQ(sim.cmd_load_bt_adapter(), std::string("7-1"));
  PB_CHECK_EQ(sim.cmd_ble_advertise("profiles/ble_keyboard.json"),
              std::string("OK C0:11:22:33:44:55"));
  // 广播中的设备进入 ble-list（GUI BLE 列表与 gs_refresh 的数据源）
  auto rows = control::execute(sim, "ble-list");
  bool seen = false;
  for (auto& r : rows)
    if (r.find("C0:11:22:33:44:55") != std::string::npos) seen = true;
  PB_CHECK(seen);
  // ble-stop（GUI 202 从选中行首取地址）→ "OK stopped"，列表随之清空
  auto rep = control::execute(sim, "ble-stop C0:11:22:33:44:55");
  PB_CHECK_EQ(rep.size(), (size_t)1);
  if (!rep.empty()) PB_CHECK_EQ(rep[0], std::string("OK stopped"));
  auto rows2 = control::execute(sim, "ble-list");
  bool gone = true;
  for (auto& r : rows2)
    if (r.find("C0:11:22:33:44:55") != std::string::npos) gone = false;
  PB_CHECK(gone);
  sim.stop();
}
