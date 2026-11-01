// 覆盖率补充（R1）：src/sim/simulator.cpp / src/sim/device_registry.cpp /
// src/control/pipe_server.cpp / src/control/commands.cpp 的未覆盖行。
// 全部走公开 API；回环 TCP（与 test_usbip_server.cpp 同一 USB/IP 协议模式）与
// 命名管道（与 test_pipe_gui.cpp 同一模式），不依赖物理硬件与真实网络。
#include "test/test_framework.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "control/commands.h"
#include "control/pipe_server.h"
#include "core/bytes.h"
#include "core/json.h"
#include "core/net.h"
#include "hci/packets.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"
#include "sim/device_registry.h"
#include "sim/simulator.h"
#include "usbip/protocol.h"

using namespace pb;

namespace {

// —— USB/IP 回环客户端辅助（协议布局同 test_usbip_server.cpp；独立命名避免跨 TU 冲突）——
void cov_send_raw(net::TcpConn& c, const std::vector<uint8_t>& v) {
  std::string err;
  if (!c.send_all(v.data(), v.size(), &err)) throw std::runtime_error("cov send: " + err);
}

std::vector<uint8_t> cov_recv_n(net::TcpConn& c, size_t n) {
  std::vector<uint8_t> v(n);
  std::string err;
  if (!c.recv_exact(v.data(), n, &err)) throw std::runtime_error("cov recv: " + err);
  return v;
}

void cov_import(net::TcpConn& c, const char* busid) {
  // op_common 新握手：u16 version(0x0111) + u16 code(OP_REQ_IMPORT) + u32 status + 32B busid
  ByteWriter w;
  w.u16be(usbip::OP_VERSION);
  w.u16be(usbip::OP_REQ_IMPORT);
  w.u32be(0);
  char b[32] = {0};
  for (size_t i = 0; busid[i] && i < 31; ++i) b[i] = busid[i];
  w.bytes((const uint8_t*)b, 32);
  cov_send_raw(c, w.buf());
  auto rep = cov_recv_n(c, 8);
  if (rep[1] != 0x11 || rep[3] != 0x03 || rep[7] != 0x00) throw std::runtime_error("cov import rejected");
  cov_recv_n(c, 312);
}

// EP0 类请求（bm=0x20 bRequest=0）携带 HCI 命令 —— Windows BT 传输层等价路径
void cov_hci_cmd(net::TcpConn& c, const std::vector<uint8_t>& cmd) {
  ByteWriter w;
  w.u32be(usbip::USBIP_CMD_SUBMIT);
  w.u32be(1);
  w.u32be(0x00070001);
  w.u32be(usbip::DIR_OUT);
  w.u32be(0);                     // ep
  w.u32be(0);                     // transfer_flags
  w.u32be((uint32_t)cmd.size());  // transfer_length
  w.u32be(0);                     // start_frame
  w.u32be(0);                     // number_of_packets
  w.u32be(0);                     // interval
  uint8_t setup[8] = {0x20, 0x00, 0, 0, 0, 0, (uint8_t)cmd.size(), 0};
  w.bytes(setup, 8);
  w.bytes(cmd.data(), cmd.size());  // data stage
  cov_send_raw(c, w.buf());
  auto ret = cov_recv_n(c, usbip::RET_SUBMIT_HDR_SIZE);
  if (ret[3] != 0x03) throw std::runtime_error("cov hci: no RET_SUBMIT");
}

// INT IN(0x81) 轮询取回 HCI 事件；无数据时服务端挂起 URB，阻塞读至事件到达
std::vector<uint8_t> cov_poll_event(net::TcpConn& c, uint32_t seq) {
  ByteWriter w;
  w.u32be(usbip::USBIP_CMD_SUBMIT);
  w.u32be(seq);
  w.u32be(0x00070001);
  w.u32be(usbip::DIR_IN);
  w.u32be(0x81);
  w.u32be(0);
  w.u32be(64);
  w.u32be(0);
  w.u32be(0);
  w.u32be(1);
  uint8_t setup[8] = {};
  w.bytes(setup, 8);
  cov_send_raw(c, w.buf());
  auto ret = cov_recv_n(c, usbip::RET_SUBMIT_HDR_SIZE);
  if (ret[3] != 0x03) throw std::runtime_error("cov poll: no RET_SUBMIT");
  int32_t len = (int32_t)(((uint32_t)ret[24] << 24) | ((uint32_t)ret[25] << 16) |
                          ((uint32_t)ret[26] << 8) | ret[27]);
  if (len <= 0) throw std::runtime_error("cov poll: empty event");
  return cov_recv_n(c, (size_t)len);
}

}  // namespace

// —— simulator.cpp:65,66,67 —— start() 全部候选端口被占 → 逐端口告警并失败
PB_TEST(cov_sim_control_start_fails_when_ports_busy) {
  net::WsaSession wsa;  // 本测试自行初始化 Winsock（不依赖其他测试先跑）
  const uint16_t base = 40150;
  std::vector<SOCKET> held;
  bool all = true;
  for (int i = 0; i < 10; ++i) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;  // 独占绑定：即使 net::TcpListener 设了 SO_REUSEADDR 也确定冲突
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)(base + i));
    if (s == INVALID_SOCKET || bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
      all = false;
      break;
    }
    held.push_back(s);
  }
  PB_CHECK(all);
  if (held.size() != 10) {  // 环境端口冲突（罕见）：仅记录失败，不再做无意义断言
    for (SOCKET s : held) closesocket(s);
    return;
  }
  sim::Simulator sim;
  std::string err;
  PB_CHECK(!sim.start(&err, base));  // base..base+9 共 10 个端口全被独占
  PB_CHECK(!err.empty());
  sim.stop();  // 失败后的 stop 必须可安全调用（server 未启动）
  for (SOCKET s : held) closesocket(s);
}

// —— simulator.cpp:83-98（ascii_to_hid_key 全分支）与 157-171（cmd_ble_type 全分支）——
// 经回环 USB/IP 作为“主机”建立 LE 连接（与 tests/integration/test_ble_e2e.cpp 相同的
// HCI 字节序列），使模拟器内部适配器的 has_connection() 为真，随后覆盖 ble-type。
PB_TEST(cov_sim_control_ble_type_typing_full_path) {
  net::WsaSession wsa;
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));  // 随机端口 + tick 线程（驱动连接完成延迟事件）
  PB_CHECK_EQ(control::execute(sim, "load-bt")[0], std::string("OK 7-1"));
  auto adv = control::execute(sim, "ble-advertise profiles/ble_keyboard.json");
  PB_CHECK_EQ(adv[0], std::string("OK C0:11:22:33:44:55"));

  auto c = net::TcpConn::connect("127.0.0.1", sim.port(), &err);
  PB_CHECK(c.is_open());
  DWORD tv = 10000;  // 兜底：任何意外都让 recv 失败 → 测试失败而非挂死
  setsockopt((SOCKET)c.raw(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
  cov_import(c, "7-1");

  // LE Create Connection（字节布局同 e2e：scan 0x60/0x30, filter 0, peer type 1, addr LSB first）
  std::vector<uint8_t> cmd = {0x0D, 0x20, 24, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
  for (int i = 0; i < 6; ++i) cmd.push_back((uint8_t)((0xC01122334455ull >> (8 * i)) & 0xFF));
  for (int i = 0; i < 12; ++i) cmd.push_back(0);
  cov_hci_cmd(c, cmd);
  auto st = cov_poll_event(c, 2);  // Command Status：命令受理，立即回
  PB_CHECK_EQ(st[0], hci::EVT_COMMAND_STATUS);
  auto cc = cov_poll_event(c, 3);  // LE Connection Complete：阻塞至 tick 线程投递（~208ms）
  PB_CHECK_EQ(cc[0], hci::EVT_LE_META);
  PB_CHECK_EQ(cc[2], hci::LE_EVT_CONNECTION_COMPLETE);
  PB_CHECK_EQ(cc[3], hci::ST_SUCCESS);

  // 已连接 —— ble-type 成功路径（协议入口）
  auto t1 = control::execute(sim, "ble-type C0:11:22:33:44:55 aZ5");
  PB_CHECK_EQ(t1[0], std::string("OK typed 3 chars"));
  // 地址非空但未广播 → 抛错分支（157-160）
  auto t2 = control::execute(sim, "ble-type C0:99:88:77:66:55 x");
  PB_CHECK_EQ(t2[0].substr(0, 3), std::string("ERR"));
  PB_CHECK(t2[0].find("no ble device") != std::string::npos);
  // 直调：覆盖 ascii→HID 全部分支（小写/大写 Shift/数字/0/空格/回车换行/退格/TAB/不可映射跳过）
  PB_CHECK_EQ(sim.cmd_ble_type("C0:11:22:33:44:55", std::string("aZ5 0\n\r\b\t@~")),
              std::string("typed 9 chars"));
  // 空 addr → 跳过设备校验直接键入（157 条件为假）
  PB_CHECK_EQ(sim.cmd_ble_type("", "hi"), std::string("typed 2 chars"));

  c.close();
  sim.stop();
}

// —— simulator.cpp:256 —— 卸下首个 BT 适配器后，设备栈重接线到剩余适配器
PB_TEST(cov_sim_control_unload_first_bt_rewires_stack) {
  sim::Simulator sim;  // 纯模型层命令，无需 usbip 服务器
  PB_CHECK_EQ(control::execute(sim, "load-bt")[0], std::string("OK 7-1"));
  PB_CHECK_EQ(control::execute(sim, "load-bt")[0], std::string("OK 7-2"));
  PB_CHECK_EQ(control::execute(sim, "unload 7-1")[0], std::string("OK unloaded 7-1"));
  // 重接线成功的判据：ble-type 报 "no ble connection"（栈在 7-2 上、只是无连接）；
  // 若未重接线（栈为空）则报 "no bt adapter loaded"
  auto t = control::execute(sim, "ble-type C0:11:22:33:44:55 hi");
  PB_CHECK_EQ(t[0].substr(0, 3), std::string("ERR"));
  PB_CHECK(t[0].find("no ble connection") != std::string::npos);
  PB_CHECK_EQ(control::execute(sim, "unload-all")[0], std::string("OK unloaded 1 device(s)"));
}

// —— commands.cpp:81 —— "detach" 分发到 cmd_detach
// bug detach-by-device 修复后的确定结果（按环境三选一）：
//   装有 usbip.exe 且 7-1 未导入（本机常态）→ "OK not attached"：解析
//   "usbip port" 未命中即无事可做，不再恒 detach --all；
//   7-1 恰被某测试台导入 → 按端口卸载 "OK detached"（真实解绑见下方
//   cov_sim_control_detach_real_kernel_unbind）；
//   未装 usbip.exe → 抛 "usbip.exe not found" → ERR。
PB_TEST(cov_sim_control_detach_via_execute) {
  sim::Simulator sim;
  auto r = control::execute(sim, "detach 7-1");
  PB_CHECK(r[0].rfind("OK detached", 0) == 0 || r[0].rfind("OK not attached", 0) == 0 ||
           r[0].rfind("ERR usbip.exe not found", 0) == 0);
}

// —— device_registry.cpp:21,22,23 —— remove 未命中（循环走完 → return false）
PB_TEST(cov_sim_control_registry_remove_miss) {
  sim::DeviceRegistry reg;
  PB_CHECK_EQ(reg.size(), 0u);
  PB_CHECK(!reg.remove("7-404"));  // 空表：循环体不执行，直接走 22-23
  auto prof = hid::HidProfile::from_json(json::parse(
      "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\",\"product\":\"v\"}"));
  auto model = std::make_shared<hid::HidModel>(prof, 7, 1);
  PB_CHECK_EQ(reg.add(model), std::string("7-1"));
  PB_CHECK(!reg.remove("7-2"));  // 非空表：迭代不命中 → 21,22,23
  PB_CHECK(reg.remove("7-1"));
  PB_CHECK(reg.find("7-1") == nullptr);
  PB_CHECK_EQ(reg.size(), 0u);
}

// —— pipe_server.cpp:26,27,28（CreateNamedPipe 失败分支）与 114（客户端空转等待分支）——
PB_TEST(cov_sim_control_pipe_fail_and_idle_wait) {
  sim::Simulator sim;
  std::string err;
  const DWORD pid = GetCurrentProcessId();

  // (a) 自建管道服务端永不回写：send_line 的 PeekNamedPipe 恒为 0 字节 → 走 Sleep(20) 空转
  //     分支直至超时；期间服务端保证不发数据，结果确定。
  std::string idle_name = "\\\\.\\pipe\\pb-cov-simctl-idle-" + std::to_string(pid);
  HANDLE srv = CreateNamedPipeA(idle_name.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
                                nullptr);
  PB_CHECK(srv != INVALID_HANDLE_VALUE);
  std::vector<std::string> replies;
  PB_CHECK(!control::CommandClient::send_line(idle_name, "status", replies, 60));
  PB_CHECK(replies.empty());

  // (b) 非法管道名（临时“目录”路径）：CreateNamedPipeA 对非管道命名空间确定失败 →
  //     告警 + Sleep(200) + continue；stop() 的唤醒 CreateFileA 对目录同样确定失败，
  //     走满 20×100ms 重试窗口，期间监听线程反复命中失败分支。
  char tmp[MAX_PATH] = {0};
  GetTempPathA(MAX_PATH, tmp);
  control::PipeServer bad(sim);
  PB_CHECK(bad.start(&err, std::string(tmp)));  // start 恒返回 true（线程内才失败）
  PB_CHECK(bad.running());

  // (c) 期间做一次真实回环（正常路径回归 + 保证失败线程已被调度）
  control::PipeServer good(sim);
  std::string good_name = "\\\\.\\pipe\\pb-cov-simctl-good-" + std::to_string(pid);
  PB_CHECK(good.start(&err, good_name));
  std::vector<std::string> rs;
  PB_CHECK(control::CommandClient::send_line(good_name, "status", rs, 3000));
  if (!rs.empty()) PB_CHECK(rs[0].rfind("OK usbip-port=", 0) == 0);

  good.stop();
  bad.stop();  // join：确定等到监听线程退出
  PB_CHECK(!bad.running());
  CloseHandle(srv);
}

// ===================== R1 追加：上一轮仍未覆盖的行 =====================
// 约束“只在文件末尾追加”，故 include 置于此处；仅下方新增测试使用。
#include <thread>

// —— commands.cpp:94-96 —— 命令实现抛异常 → execute 的 catch → "ERR <what>"。
// unload 未知 busid：cmd_unload 在 reg.find 未命中时确定抛错（simulator.cpp:232，
// 纯内存路径，无子进程/网络）。
PB_TEST(cov_sim_control_execute_err_path) {
  sim::Simulator sim;
  auto r = control::execute(sim, "unload 7-404");
  PB_CHECK_EQ(r.size(), 1u);
  PB_CHECK_EQ(r[0], std::string("ERR unknown busid 7-404"));
}

// —— simulator.cpp:339-340 —— cmd_attach 两种语法均失败 → 抛 "usbip attach failed"。
// 模拟器不 start（port_==0，不与任何真实 usbip 服务端交互）。本机装有 usbip.exe 时
// 两条命令都在参数解析阶段失败（实测 cmd.exe 包装后 rc=106 "--bus-id is required" /
// rc=109 "-p 0 not expected"，错误仅写 stderr、无网络交互），确定走到 throw；
// 未装 usbip.exe 的机器则在 simulator.cpp:316 抛 "usbip.exe not found…"，同样确定。
PB_TEST(cov_sim_control_attach_fails_without_server) {
  sim::Simulator sim;
  bool threw = false;
  std::string msg;
  try {
    (void)sim.cmd_attach("9-9");
  } catch (const std::exception& e) {
    threw = true;
    msg = e.what();
  }
  PB_CHECK(threw);
  // 装机环境："usbip attach failed for 9-9"；裸机环境："usbip.exe not found…"。
  PB_CHECK(msg.find("usbip") != std::string::npos);
}

// —— pipe_server.cpp:114 —— send_line 等待应答时空转分支（PeekNamedPipe 0 字节
// → Sleep(20)）。上轮 idle 测试未命中此行的原因：其服务端只 CreateNamedPipeA 而
// 从未 ConnectNamedPipe，客户端 CreateFileA 因无监听实例连不上，40 次重试后从
// 94 行直接返回、从未进入 peek 循环。本测试用真实监听的服务端线程：接受连接、
// 读走请求行、然后保持连接但永不回写 —— 客户端首轮 PeekNamedPipe 必为 0 字节
// （服务端保证不发任何数据），必然进入 Sleep(20) 分支直至 120ms 超时返回 false。
PB_TEST(cov_sim_control_send_line_idle_sleep_branch) {
  const std::string name =
      "\\\\.\\pipe\\pb-cov-simctl-idle2-" + std::to_string(GetCurrentProcessId());
  HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  PB_CHECK(done != nullptr);
  std::thread srv([&name, done] {
    HANDLE pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096,
                                   4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;
    ConnectNamedPipe(pipe, nullptr);  // 阻塞至 send_line 的 CreateFileA 连接
    char buf[64];
    DWORD n = 0;
    ReadFile(pipe, buf, sizeof(buf), &n, nullptr);  // 取走请求行（客户端连接后立刻写）
    WaitForSingleObject(done, 5000);                // 持连接、永不回写（5s 仅为兜底）
    CloseHandle(pipe);
  });
  std::vector<std::string> replies;
  PB_CHECK(!control::CommandClient::send_line(name, "status", replies, 120));
  PB_CHECK(replies.empty());  // 服务端永不回写 → 无完整应答行
  SetEvent(done);
  srv.join();
  CloseHandle(done);
}

// ===================== R2 追加：第 2 轮未覆盖行 =====================
// 约束“只在文件末尾追加”，故本节所需 include 置于此处；仅下方新增测试使用。
#include <atomic>

// —— simulator.cpp:331-336（_popen stdout 消费循环）——
// 原版（R2）曾以 busid="x --help" 实际注入参数使 usbip.exe 打印 usage 后
// rc=0，借以覆盖 _popen stdout 循环与正常 return 行。缺陷修复批 bug
// attach-inject 落地 busid 白名单 ^\d+-\d+$ 后，该注入路径被关闭：非法 busid
// 在构造命令串之前即被拒绝（校验先于 find_usbip_exe/_popen，装/未装
// usbip.exe 的机器上均确定）。本测试改为锚定拒绝行为；_popen 循环行在装有
// usbip.exe 的机器上仍由 cov_sim_control_attach_fails_without_server
//（合法 "9-9"）与 R4 真实路径测试覆盖。
PB_TEST(cov_sim_control_attach_usage_stdout_normal_return) {
  sim::Simulator sim;
  bool threw = false;
  std::string msg;
  try {
    (void)sim.cmd_attach("x --help");
  } catch (const std::exception& e) {
    threw = true;
    msg = e.what();
  }
  PB_CHECK(threw);
  PB_CHECK(msg.find("invalid busid") != std::string::npos);  // 白名单拒绝，非环境报错
}

// —— pipe_server.cpp:114 —— send_line 应答等待循环中 `} else {` 行。
// 本仓库覆盖率报告实证（CoverageReport-2026-11-01-05h31m32s/Modules/
// phantombt-tests/server.cpp.html:180/184）：`} else {` 行的覆盖项挂在 if 真
// 分支的收尾跳转上 —— 即“Peek 到数据、:110 ReadFile 成功、:113 因 acc 不以
// '\n' 结尾而不 break、正常落出 if 块”时才执行。上轮 idle 测试只让 Peek 恒为
// 0 字节（仅覆盖 else 体 :115 的 Sleep(20)），故 :114 仍缺。
// 本测试：服务端接受连接、读走请求行、写一段无 '\n' 结尾的部分应答后保持
// 静默 —— 客户端必然在某轮 Peek 到这些字节、消费后不满足 :113 的 break
// 条件而落出 if 块（:114 执行），随后 Peek 0 字节进入 else 体直至超时。
// 全程事件驱动（ConnectNamedPipe/ReadFile/WriteFile 皆为阻塞同步点），
// 无测试侧 sleep；结果确定：永不出现 '\n' → 永无完整应答行 → 返回 false。
PB_TEST(cov_sim_control_send_line_partial_reply_no_newline) {
  const std::string name =
      "\\\\.\\pipe\\pb-cov-simctl-part-" + std::to_string(GetCurrentProcessId());
  HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  PB_CHECK(done != nullptr);
  std::atomic<bool> wrote_partial{false};
  std::thread srv([&name, done, &wrote_partial] {
    HANDLE pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096,
                                   4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;
    ConnectNamedPipe(pipe, nullptr);               // 阻塞至 send_line 的 CreateFileA
    char buf[64];
    DWORD n = 0;
    ReadFile(pipe, buf, sizeof(buf), &n, nullptr);  // 取走请求行（客户端连接后立刻写）
    static const char kPartial[] = "OK partial reply without newline";  // 无 '\n'
    DWORD w = 0;
    if (WriteFile(pipe, kPartial, (DWORD)(sizeof(kPartial) - 1), &w, nullptr))
      wrote_partial = true;
    WaitForSingleObject(done, 10000);  // 持连接、不再发任何字节（10s 仅兜底）
    CloseHandle(pipe);
  });
  std::vector<std::string> replies;
  PB_CHECK(!control::CommandClient::send_line(name, "status", replies, 1000));
  PB_CHECK(replies.empty());       // 无 '\n' 终结 → 永无完整应答行
  PB_CHECK(wrote_partial.load());  // 部分应答确实已写入（否则本测试不成立）
  SetEvent(done);
  srv.join();
  CloseHandle(done);
}

// —— commands.cpp:94-97 —— execute 的异常处理收尾（:96/:97 为 catch 块与
// 函数的右括号行）。上轮已有 "unload 7-404"（命令实现深层抛出）一种形态；
// 本测试补两种新形态，扩大异常完成路径的覆盖面：
//   (a) :84 from_hex —— 在 execute 自身 try 块内的直接调用点抛出
//       （"zz" → bytes.cpp:30 "from_hex: invalid digit"，纯内存）；
//   (b) ble-advertise 打不开文件 → ble_device.cpp:54 "cannot open"（无 IO 副作用，
//       仅尝试打开不存在的路径）。
// 全仓 grep 实证：src 内可达的抛出类型全部派生自 std::exception（唯一自定义
// 异常 ControlStall 亦继承 std::runtime_error，src/usb/control.h:38），不存在
// “catch 类型不匹配继续上抛”的可注入路径。
PB_TEST(cov_sim_control_execute_catch_direct_and_file_throw) {
  sim::Simulator sim;
  auto r1 = control::execute(sim, "inject 7-1 zz");
  PB_CHECK_EQ(r1.size(), 1u);
  PB_CHECK_EQ(r1[0].substr(0, 4), std::string("ERR "));
  PB_CHECK(r1[0].find("from_hex") != std::string::npos);
  auto r2 = control::execute(sim, "ble-advertise no-such-dir/no-such-profile.json");
  PB_CHECK_EQ(r2.size(), 1u);
  PB_CHECK_EQ(r2[0].substr(0, 4), std::string("ERR "));
  PB_CHECK(r2[0].find("cannot open") != std::string::npos);
}

// ===================== R3 追加：第 3 轮未覆盖行 =====================
// 结论（依据见 uncoverable 申报）：本轮目标行 simulator.cpp:340 与
// commands.cpp:96-97 是编译器生成的不可达机器码，任何测试都无法执行 ——
// 用本轮门禁产物（build/debug/tests/phantombt-tests.exe + .pdb，2026-11-01
// 08:39 构建）经 llvm-pdbutil 行表 + llvm-objdump 全 .text 反汇编证实：
//   :340 = VMA 0x1401126C4（__ehvec_dtor 析构 cmds[2] + 析构 exe + sret 收
//   尾）。:336 正常 return 直接跳 0x1401126EB 收尾、绕开该块；其唯一的顺
//   序前驱是 noreturn 的 _CxxThrowException(:339) 之后的死代码
//   0x1401126B6；全 .text（1,578,036 条指令）中零分支引用 0x1401126C4。
//   :96 = catch funclet 0x140396E1A，仅为“catch 体落入结尾不 return”加载
//   续体地址 —— funclet 无条件走 :95 续体 0x140124504 并 jmp 跳过该加载；
//   :97 = 0x140124518（析构 args + 收尾），唯一入口是蹦床 0x140124502，
//   而蹦床只被 :96 的死加载引用。三个出口语义路径（:336 return、:339
//   throw、execute 全分支 + catch :94-95）在 08:39 门禁 coverage.xml 中
//   均已有命中。
// 本节新增测试不为覆盖上述死代码行，而是把与 :340 相邻的唯一真实出口
// （:336 正常 return）钉死。原版（R3）以 busid="1 & exit 0" 的 cmd 元字符
// 注入使外层 cmd.exe 退出码无条件为 0（实测确实返回 "attached 1 & exit 0"）。
// 缺陷修复批 bug attach-inject 落地 busid 白名单 ^\d+-\d+$ 后该注入路径被
// 关闭：cmd 元字符输入在构造命令串之前即被拒绝（校验先于 find_usbip_exe/
// _popen，装/未装 usbip.exe 的机器上均确定）。本测试改为锚定拒绝行为；
// 正常 return 行在装有 usbip.exe + 管理员的环境由 R4 真实路径测试覆盖。
PB_TEST(cov_sim_control_attach_normal_return_via_exit0_injection) {
  sim::Simulator sim;
  bool threw = false;
  std::string msg;
  try {
    (void)sim.cmd_attach("1 & exit 0");
  } catch (const std::exception& e) {
    threw = true;
    msg = e.what();
  }
  PB_CHECK(threw);
  PB_CHECK(msg.find("invalid busid") != std::string::npos);  // 白名单拒绝，非环境报错
}

// ===================== 缺陷修复锚定（R4） =====================
// （PB_TEST 名即 C 标识符，bug id 中的 '-' 映射为 '_'）

// —— bug id "snoop-cmd"（评审 #46）：实施计划要求的 `snoop <path>|off` 控制命令 ——
// 缺陷：抓包路径硬编码在 cmd_load_bt_adapter 内（integration-results/hci.snoop），
// 无法重定向/关闭。锚定：off 关闭写者并保持关闭（跨后续 load-bt 不复活）；
// snoop <path> 重定向且对已加载适配器真实挂钩（回环 EP0 发 HCI Reset，控制器
// handle_command 入口的 snoop_command 钩子必须计入 record_count）；坏路径 ERR。
PB_TEST(bug_snoop_cmd_redirect_and_off) {
  net::WsaSession wsa;
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  PB_CHECK_EQ(control::execute(sim, "load-bt")[0], std::string("OK 7-1"));

  // off：无论 load-bt 的默认抓包是否打开，off 后必须关闭
  PB_CHECK_EQ(control::execute(sim, "snoop off")[0], std::string("OK snoop off"));
  PB_CHECK(!sim.snoop().is_open());

  // 重定向到仓库内相对路径（测试 CWD = 仓库根，无空格，不碰系统临时目录）
  const std::string path =
      "integration-results/bug-snoop-cmd-" + std::to_string(GetCurrentProcessId()) + ".snoop";
  DeleteFileA(path.c_str());  // 清残留，保证是本次新建
  PB_CHECK_EQ(control::execute(sim, "snoop " + path)[0], std::string("OK snoop " + path));
  PB_CHECK(sim.snoop().is_open());
  PB_CHECK_EQ(sim.snoop().record_count(), 0u);

  // 重定向后的抓包必须真实挂钩：回环 EP0 → HCI Reset（opcode 0x0C03）→
  // MiniController::handle_command 入口 snoop_command（controller.cpp:331）计 1 条
  auto c = net::TcpConn::connect("127.0.0.1", sim.port(), &err);
  PB_CHECK(c.is_open());
  DWORD tv = 10000;  // 兜底：任何意外都让 recv 失败 → 测试失败而非挂死
  setsockopt((SOCKET)c.raw(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
  cov_import(c, "7-1");
  cov_hci_cmd(c, {0x03, 0x0C, 0x00});  // HCI Reset（非 pending 类命令 → Command Complete）
  auto evt = cov_poll_event(c, 2);
  PB_CHECK_EQ(evt[0], hci::EVT_COMMAND_COMPLETE);
  PB_CHECK(sim.snoop().record_count() >= 1u);  // 命令已落到新路径的写者
  c.close();

  // off 再次关闭；off 状态跨 load-bt 保持（新适配器不得复活默认抓包）
  PB_CHECK_EQ(control::execute(sim, "snoop off")[0], std::string("OK snoop off"));
  PB_CHECK(!sim.snoop().is_open());
  PB_CHECK_EQ(control::execute(sim, "load-bt")[0], std::string("OK 7-2"));
  PB_CHECK(!sim.snoop().is_open());

  // 坏路径（不存在的目录）→ ERR；缺参数 → ERR；help 列出 snoop
  auto bad = control::execute(sim, "snoop no-such-dir-" + std::to_string(GetCurrentProcessId()) +
                                       "/x.snoop");
  PB_CHECK_EQ(bad[0].substr(0, 4), std::string("ERR "));
  PB_CHECK(bad[0].find("cannot open") != std::string::npos);
  PB_CHECK_EQ(control::execute(sim, "snoop")[0].substr(0, 3), std::string("ERR"));
  PB_CHECK(control::execute(sim, "help")[0].find("snoop") != std::string::npos);
  sim.stop();
}

// —— bug id "pipe-stop-hang"（评审 #40）：有阻塞客户端时 stop() 不得无限挂起 ——
// 缺陷：客户端保持连接且不再发数据时，监听线程阻塞在 client_loop 的 ReadFile；
// 单实例管道被占用使 stop() 的唤醒 CreateFileA 恒 ERROR_PIPE_BUSY，join 无超时
// 也无 CancelSynchronousIo → stop()/析构永久挂起。
// 场景：连上 → 完成一次问答（证明服务端已进入 client_loop）→ 客户端静默持连接 →
// stop() 必须在有限时间内返回（修复：CancelSynchronousIo 取消阻塞读）。
PB_TEST(bug_pipe_stop_hang_stop_returns_with_connected_client) {
  sim::Simulator sim;  // status 命令不依赖 usbip 服务器，纯模型层
  std::string err;
  const std::string name =
      "\\\\.\\pipe\\pb-bug-stop-hang-" + std::to_string(GetCurrentProcessId());
  // 故意 new（而非栈对象）：stop() 挂起时泄漏本对象，避免析构与挂起中的 stop
  // 并发触碰 threads_（std::thread 析构 joinable → terminate 会吞掉失败报告）。
  auto* server = new control::PipeServer(sim);
  PB_CHECK(server->start(&err, name));

  // 连上（等服务端建好管道实例）
  HANDLE client = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 100 && client == INVALID_HANDLE_VALUE; ++i) {
    client = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         0, nullptr);
    if (client == INVALID_HANDLE_VALUE) Sleep(20);
  }
  PB_CHECK(client != INVALID_HANDLE_VALUE);

  // 一问一答：收到完整应答行 ⇒ 服务端已在 client_loop 并回到 ReadFile 等待下一行
  static const char kReq[] = "status\n";
  DWORD w = 0;
  PB_CHECK(WriteFile(client, kReq, (DWORD)(sizeof(kReq) - 1), &w, nullptr));
  std::string acc;
  char buf[512];
  DWORD n = 0;
  while (acc.find('\n') == std::string::npos) {
    if (!ReadFile(client, buf, sizeof(buf), &n, nullptr) || n == 0) break;
    acc.append(buf, buf + n);
  }
  PB_CHECK(acc.rfind("OK usbip-port=", 0) == 0);  // 应答完整送达
  // 此处起客户端静默持连接不发数据 → 服务端阻塞在 ReadFile（正是缺陷场景）

  // 看门狗：stop() 必须在 15s 内返回（修复后实测 <1s；修复前永久挂起）
  std::atomic<bool> stopped{false};
  std::thread watcher([&] {
    server->stop();
    stopped = true;
  });
  for (int i = 0; i < 1500 && !stopped.load(); ++i) Sleep(10);
  PB_CHECK(stopped.load());  // stop() 在有阻塞客户端时返回 —— 本缺陷的判据
  if (stopped.load()) {
    watcher.join();
    PB_CHECK(!server->running());
    delete server;  // stop 已完成、threads_ 已清：析构安全（二次 stop 立即返回）
  } else {
    watcher.detach();  // 泄漏 server：进程退出回收；绝不并发析构
  }
  CloseHandle(client);
}

// ===================== 缺陷修复批（bug_btsnoop-ts）：src/sim/btsnoop.cpp =====================
// 缺陷：(1) now_us_since_epoch 把“自 1601 的 FILETIME/10”直接当 unix µs 平移到
// btsnoop 纪元，多算了 1601→1970 的 11644473600 秒（Wireshark 里记录时间在
// ~3600 年代）；(2) 记录 flags 恒 0，控制器→主机方向的事件/ACL 也被标成
// 主机→控制器。正确行为：时间戳 = unix µs（1970 纪元）+ 0x00E03AB1A6764000
//（btsnoop 0000-01-01 纪元）；flags bit0：0=主机→控制器（命令 / ACL OUT）、
// 1=控制器→主机（事件 / ACL IN）。时间基准用 CRT time()（1970 纪元，与实现的
// GetSystemTimeAsFileTime 相互独立），秒粒度由 ±2s 余量吸收。
// 约束“只在文件末尾追加”，故本节 include 置于此处；仅下方新增测试使用。
#include <ctime>

#include "sim/btsnoop.h"

namespace {

// 解析 btsnoop 文件：16B 文件头 + N ×（24B 记录头 + incl_len 数据）
struct BugSnoopRec {
  uint32_t orig_len = 0, incl_len = 0, flags = 0, cum_drops = 0;
  uint64_t ts = 0;
  std::vector<uint8_t> data;
};

std::vector<BugSnoopRec> bug_parse_snoop(const std::string& path) {
  std::vector<BugSnoopRec> recs;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return recs;
  std::vector<uint8_t> file;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
  std::fclose(f);
  if (file.size() < 16) return recs;
  ByteReader r(file);
  r.skip(16);  // "btsnoop\0" + version(4) + datalink(4)
  while (r.remaining() >= 24) {
    BugSnoopRec rec;
    rec.orig_len = r.u32be();
    rec.incl_len = r.u32be();
    rec.flags = r.u32be();
    rec.cum_drops = r.u32be();
    rec.ts = r.u64be();
    rec.data = r.bytes(rec.incl_len);
    if (!r.ok()) return {};  // 头部长度超出文件：整体无效
    recs.push_back(std::move(rec));
  }
  return recs;
}

}  // namespace

// 时间戳纪元：记录 ts 换回 unix µs 后必须落在写入时刻附近（±2s）。
// 现缺陷实现偏差 +11644473600s（≈369 年），必然越界。
PB_TEST(bug_btsnoop_ts_record_timestamps_in_unix_epoch) {
  sim::BtSnoopWriter w;
  std::string path = "integration-results/bug-btsnoop.snoop";
  PB_CHECK(w.open(path));
  std::time_t t0 = std::time(nullptr);
  w.hci_command({0x03, 0x0C, 0x00});                                   // HCI Reset
  w.hci_event(hci::build_command_complete(1, hci::CMD_RESET, {0x00}));  // CC 事件
  std::time_t t1 = std::time(nullptr);
  PB_CHECK_EQ(w.record_count(), 2u);
  w.close();

  auto recs = bug_parse_snoop(path);
  PB_CHECK_EQ(recs.size(), 2u);
  constexpr uint64_t kEpochUs = 0x00E03AB1A6764000ull;  // btsnoop 纪元 0000-01-01
  for (const auto& rec : recs) {
    PB_CHECK(rec.ts >= kEpochUs);  // 未加纪元常量时此处即失败（避免下溢出假通过）
    uint64_t unix_us = rec.ts - kEpochUs;
    PB_CHECK(unix_us <= (uint64_t)(t1 + 2) * 1000000ull);          // 不早于写入结束 +2s
    PB_CHECK(unix_us + 2ull * 1000000ull >= (uint64_t)t0 * 1000000ull);  // 不晚于写入开始 −2s
  }
}

// 方向 flags：命令(0x01)=0 / 事件(0x04)=1 / ACL 主机→控制器(0x02)=0 / ACL 控制器→主机=1
PB_TEST(bug_btsnoop_ts_record_direction_flags) {
  sim::BtSnoopWriter w;
  std::string path = "integration-results/bug-btsnoop-flags.snoop";
  PB_CHECK(w.open(path));
  w.hci_command({0x03, 0x0C, 0x00});                                   // 主机→控制器
  w.hci_event(hci::build_command_complete(1, hci::CMD_RESET, {0x00}));  // 控制器→主机
  hci::AclFrame fr{1, hci::ACL_PB_START, {1, 2}};
  w.acl_from_host(fr);  // 主机→控制器
  w.acl_to_host(fr);    // 控制器→主机
  PB_CHECK_EQ(w.record_count(), 4u);
  w.close();

  auto recs = bug_parse_snoop(path);
  PB_CHECK_EQ(recs.size(), 4u);
  const uint32_t want_flags[4] = {0, 1, 0, 1};  // cmd / evt / acl_out / acl_in
  const uint8_t want_h4[4] = {0x01, 0x04, 0x02, 0x02};
  for (size_t i = 0; i < 4 && i < recs.size(); ++i) {
    PB_CHECK_EQ(recs[i].flags, want_flags[i]);          // 方向位（现缺陷恒 0）
    PB_CHECK_EQ(recs[i].cum_drops, 0u);
    PB_CHECK_EQ(recs[i].orig_len, recs[i].incl_len);    // 不截断
    PB_CHECK_EQ(recs[i].data.size(), (size_t)recs[i].incl_len);
    PB_CHECK_EQ(recs[i].data[0], want_h4[i]);           // H4 类型不因修复改变
  }
}

// ===================== 缺陷修复批（bug attach-inject）：simulator.cpp ==========
// busid 原样内插进 _popen 命令串（cmd.exe /c 解释），元字符即命令注入：上方
// R2/R3 覆盖测试曾实证 "x --help" / "1 & exit 0" 能改变子进程行为（并据以覆盖
// _popen stdout 循环/正常 return 行）。缺陷 attach-inject 要求在构造命令串
// 之前以白名单 ^\d+-\d+$ 拒绝一切非法输入；校验先于 find_usbip_exe/_popen，
// 拒绝路径在装/未装 usbip.exe 的机器上均确定（本机装有 usbip.exe）。
PB_TEST(bug_attach_inject_busid_whitelist_rejects_non_busid_input) {
  sim::Simulator sim;
  const char* bad[] = {
      "1 & exit 0",  // cmd 元字符注入（R3 曾实测可执行）
      "x --help",    // 参数注入（R2 曾实测可执行）
      "7-1;calc",    // 分号拼接
      "7 - 1",       // 空格
      "7-",          // 缺尾部数字
      "-1",          // 缺头部数字
      "7--1",        // 双连字符
      "7-1-2",       // 双连字符
      "",            // 空
      "7-1\"",       // 引号破坏外层引用结构
  };
  for (auto* b : bad) {
    bool threw = false;
    std::string msg;
    try {
      (void)sim.cmd_attach(b);
    } catch (const std::exception& ex) {
      threw = true;
      msg = ex.what();
    }
    PB_CHECK(threw);
    PB_CHECK(msg.find("invalid busid") != std::string::npos);  // 白名单拒绝
  }
  // 合法格式（注册表分配形如 "7-13"）通过白名单：不再以 "invalid busid" 拒绝。
  // 不 start 服务端，后续成败取决于 usbip.exe/服务端（与本缺陷无关）。
  std::string msg;
  try {
    (void)sim.cmd_attach("9-9");
  } catch (const std::exception& ex) {
    msg = ex.what();
  }
  PB_CHECK(msg.find("invalid busid") == std::string::npos);
}

// ===================== R4 追加：控制命令真实路径（盘点矩阵 3 行） =====================
// 前几轮只覆盖 attach/detach/sys-bt 的失败与注入路径；本节补真实路径（本机装有
// usbip-win2 usbip.exe 0.9.8.0 + usbip2_ude 驱动 Running + 管理员 + 物理 BT 电台，
// 实测 2026-11-01 会话）。前置缺失的环境（CI runner / 裸机）按既有惯例提前
// return（记 0 检查通过，同 cov_sim_control_attach_usage_stdout_normal_return）。
// busid 一律用注册表真实分配的 "7-1"（^\d+-\d+$），与本文件 bug 批次的白名单
// 修复兼容：无论白名单落地与否均走真实路径。
#include <cstdlib>

#include "core/bt_radio.h"

namespace {

// 当前进程是否提权（sys-bt 状态变更与 usbip attach 均需要；run-m0.ps1 真机段
// 同样以 admin 门控）。
bool cov_r4_elevated() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION te{};
  DWORD n = 0;
  BOOL ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n);
  CloseHandle(tok);
  return ok && te.TokenIsElevated != 0;
}

// 运行 usbip.exe 并捕获 stdout（与生产 cmd_attach 的 _popen 同一机制）。
std::string cov_r4_usbip(const std::string& exe, const std::string& args) {
  FILE* f = _popen(("\"" + exe + "\" " + args).c_str(), "r");
  if (!f) return "";
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), f)) out += buf;
  _pclose(f);
  return out;
}

// vhci 内核驱动是否就绪。实机实证（2026-11-01，usbip.exe 1,514,256 字节版）：
// 驱动未装/未加载时 `usbip.exe port` 立即 rc=1、stderr "error: VHCI device not
// found, driver not loaded?"、stdout 为空；就绪时 rc=0。真实内核导入/解绑测试
// 以此守卫：usbip.exe 在而 vhci 缺失的机器（本门禁宿主）上应跳过而非 FAIL——
// attach 恒 rc=1 重试到抛错，那不是产品缺陷。
bool cov_r4_vhci_ready(const std::string& exe) {
  FILE* f = _popen(("\"" + exe + "\" port").c_str(), "r");
  if (!f) return false;
  char buf[512];
  while (fgets(buf, sizeof(buf), f)) {}
  return _pclose(f) == 0;
}

std::vector<std::string> cov_r4_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n' || c == '\r') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// `usbip.exe port` 输出中 "Port NN:" 头的个数 = 当前内核导入设备数。
// （实机探测：无导入时输出为空；有导入时每设备一个 "Port 01: device in use …"
// 块。本会话实测到本机 vhci 可能同时持有其他 bench 的导入，见下方 detach 守卫。）
int cov_r4_import_count(const std::string& port_text) {
  int n = 0;
  for (auto& l : cov_r4_lines(port_text))
    if (l.rfind("Port ", 0) == 0) ++n;
  return n;
}

// 找 marker（如 "usbip://127.0.0.1:3425/7-1"）所在导入块的 hub 端口号；无则 -1。
int cov_r4_import_port_of(const std::string& port_text, const std::string& marker) {
  auto lines = cov_r4_lines(port_text);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find(marker) == std::string::npos) continue;
    for (size_t j = i; j-- > 0;)
      if (lines[j].rfind("Port ", 0) == 0) return std::atoi(lines[j].c_str() + 5);
  }
  return -1;
}

}  // namespace

// —— 矩阵行 1：控制命令 attach（真机 usbip 挂载）的真实成功路径 ——
// 此前 attach 测试要么无 server 抛错、要么 --help/& exit 0 注入让 rc=0；真正的
// “usbip.exe 与内核 vhci 完成导入、rc==0 → simulator.cpp return” 从未自动化
// （仅 run-m0.ps1 真机段，CI 明确跳过）。本测试无需外部真机：Simulator 自带
// usbip 服务器监听回环，cmd_attach 驱动真 usbip.exe 把设备内核侧挂到我们自己
// 身上。实测（2026-11-01，phantombt.exe serve --port 3425 + cmd "attach 7-1"）：
//   返回 "OK attached 7-1"；`usbip.exe port` 列出
//   "Port 01: device in use at Full Speed(12Mbps) / 1209:b007
//     -> usbip://127.0.0.1:3425/7-1"；netstat 显示 PID 4（内核）与 serve
//   建立 ESTABLISHED 连接。
// 清理用 `usbip detach -p <自己的口>` 精确摘除 —— 绝不用 --all：本机可能同时
// 挂着其他 usbip bench 的导入（本会话实证 phantom-audio 的 uacsimd 曾在 3240
// 端口持有 1d50:6222 活动导入），--all 会误伤他人状态。
PB_TEST(cov_sim_control_attach_real_kernel_loopback) {
  std::string exe = sim::Simulator::find_usbip_exe();
  if (exe.empty() || !cov_r4_elevated()) return;  // CI/裸机/未提权：无法走真实路径
  if (!cov_r4_vhci_ready(exe)) return;  // vhci 驱动缺失：attach 恒败非产品缺陷，跳过
  net::WsaSession wsa;
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));  // 随机回环端口（避开常驻 3240 的其他 bench）
  PB_CHECK_EQ(control::execute(sim, "load-hid profiles/gamepad.json")[0], std::string("OK 7-1"));

  const std::string marker = "usbip://127.0.0.1:" + std::to_string(sim.port()) + "/7-1";
  bool attached = false;
  try {
    std::string r = sim.cmd_attach("7-1");            // 真 usbip.exe + 内核 vhci 导入
    PB_CHECK_EQ(r, std::string("attached 7-1"));      // 真实 rc==0 成功出口
    attached = r == "attached 7-1";
  } catch (const std::exception& e) {
    PB_CHECK_EQ(std::string("attach threw: ") + e.what(), std::string("attached 7-1"));
  }
  if (attached) {
    std::string ports = cov_r4_usbip(exe, "port");
    PB_CHECK(ports.find(marker) != std::string::npos);  // 内核侧确实导入成功
    int p = cov_r4_import_port_of(ports, marker);
    PB_CHECK(p >= 1 && p <= 255);  // 能解析出我们的 hub 口号
    if (p >= 1) {
      cov_r4_usbip(exe, "detach -p " + std::to_string(p));  // 只摘自己的口
      PB_CHECK(cov_r4_usbip(exe, "port").find(marker) ==
               std::string::npos);  // 内核解绑生效
    }
  }
  sim.stop();
}

// —— 矩阵行 2：控制命令 detach（usbip 解绑）的真实内核解绑 ——
// 矩阵行盘点时实现还是固定 `detach --all` 不区分 busid、仅断言分发文本；本轮
// 并行的 bug detach-by-device 修复已改为按设备 `usbip detach -p <n>`（解析
// `usbip port` 输出，simulator.cpp cmd_detach）。本测试在真机上钉死新语义：
//   (a) 未导入时 cmd_detach 返回 "not attached"（无操作；旧 --all 实现恒返回
//       "detached"，回退即在此失败）；
//   (b) 真挂载自己的设备后 cmd_detach 返回 "detached"，且 `usbip.exe port`
//       中我们的导入真实消失（实测 detach 后 marker 不再出现）；
//   (c) 机器上其他 bench 的既有导入原样保留 —— 本会话实证本机常驻
//       phantom-audio 的 uacsimd（3240 端口，1d50:6222 内核导入，与 PID 4
//       ESTABLISHED）；若实现回退成 detach --all，计数断言在此失败。
// 无需 vhci 空闲守卫：按设备摘除不影响他人。
PB_TEST(cov_sim_control_detach_real_kernel_unbind) {
  std::string exe = sim::Simulator::find_usbip_exe();
  if (exe.empty() || !cov_r4_elevated()) return;  // CI/裸机/未提权：无法走真实路径
  net::WsaSession wsa;
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  PB_CHECK_EQ(control::execute(sim, "load-hid profiles/gamepad.json")[0], std::string("OK 7-1"));
  const std::string marker = "usbip://127.0.0.1:" + std::to_string(sim.port()) + "/7-1";
  PB_CHECK_EQ(sim.cmd_detach("7-1"), std::string("not attached"));  // (a) 未导入 → 无操作（纯软件，不依赖 vhci）
  if (!cov_r4_vhci_ready(exe)) {  // vhci 驱动缺失：真实导入/解绑段跳过（非产品缺陷）
    sim.stop();
    return;
  }
  const int foreign = cov_r4_import_count(cov_r4_usbip(exe, "port"));  // 他人既有导入
  bool attached = false;
  try {
    std::string r = sim.cmd_attach("7-1");
    PB_CHECK_EQ(r, std::string("attached 7-1"));
    attached = r == "attached 7-1";
  } catch (const std::exception& e) {
    PB_CHECK_EQ(std::string("attach threw: ") + e.what(), std::string("attached 7-1"));
  }
  if (attached) {
    PB_CHECK_EQ(cov_r4_import_count(cov_r4_usbip(exe, "port")),
                foreign + 1);                                // 挂载后 = 他人 + 我们的
    PB_CHECK_EQ(sim.cmd_detach("7-1"), std::string("detached"));  // (b) 真路径
    std::string after = cov_r4_usbip(exe, "port");
    PB_CHECK(after.find(marker) == std::string::npos);  // (b) 我们的导入真实消失
    PB_CHECK_EQ(cov_r4_import_count(after), foreign);   // (c) 他人导入原样保留
  }
  sim.stop();
}

// —— 矩阵行 3：控制命令 sys-bt（物理电台 disable/enable）的真实开关 ——
// 既有断言只有 list 形状与 bad-op ERR；disable/enable 真实翻转从未自动化。本机
// 具备管理员 + 物理电台（Intel USB\VID_8087&PID_0026）。实测（2026-11-01，
// serve + cmd 管道）：`sys-bt enable` → "enabled 1 radio(s)" 且 list 变
// "[enabled]  RADIO …"；`sys-bt disable` → "disabled 1 radio(s)" 且变
// "[disabled] RADIO …"。本测试直调 cmd_sys_bt，以 btradio::list_radios() 的
// Device Manager 真实状态翻转为准断言，并保证回到测试前基线（RAII：任何失败/
// 异常路径都把电台恢复到初始 enabled/disabled 状态）。
//
// ⚠ 仅在显式 opt-in（环境变量 PB_REAL_SYS_BT=1）时执行。真实翻转走
// SetupDiCallClassInstaller(DICS_DISABLE)：射频正被占用（如蓝牙外设在连）时
// 驱动卸载会无限期阻塞——2026-11-01 门禁宿主实证：提权 + 物理电台在位，
// 该调用 600s 门禁超时内未返回（套件挂死根因），且中途被杀会把宿主蓝牙硬件
// 遗留在 disabled 状态。与 ci.yml 头注一致（真机段由 run-m*.ps1 单独门控，
// 单测套件不碰真实硬件）。无电台（CI）、未提权或未 opt-in 则 return（无电台
// 报错分支已由 gs_sys_bt_* 覆盖；list/bad-op 分支已由纯软件测试覆盖）。
PB_TEST(cov_sim_control_sys_bt_real_toggle_roundtrip) {
  if (!cov_r4_elevated()) return;  // 状态变更需管理员（bt_radio.h 注释）
  if (GetEnvironmentVariableA("PB_REAL_SYS_BT", nullptr, 0) == 0)
    return;  // 无人值守门禁默认跳过：真实翻转宿主硬件不可控（见上注）
  bool any_physical = false, any_enabled = false;
  for (auto& r : btradio::list_radios())
    if (r.physical) {
      any_physical = true;
      any_enabled = any_enabled || !r.disabled;
    }
  if (!any_physical) return;  // CI/无物理电台

  sim::Simulator sim;  // cmd_sys_bt 纯 SetupAPI，无需 start
  struct Restore {     // 无论断言成败/抛异常，结束前恢复到基线状态
    sim::Simulator* s;
    bool enable;
    ~Restore() {
      try {
        (void)s->cmd_sys_bt(enable ? "enable" : "disable");
      } catch (...) {
      }
    }
  } restore{&sim, any_enabled};

  auto all_physical = [](bool disabled) {  // 真实翻转以设备枚举状态为准
    for (auto& r : btradio::list_radios())
      if (r.physical && r.disabled != disabled) return false;
    return true;
  };

  // 从当前状态翻到另一侧再翻回：两个方向都是真实状态变更（实测枚举立即生效）
  if (any_enabled) {
    PB_CHECK(sim.cmd_sys_bt("disable").rfind("disabled ", 0) == 0);
    PB_CHECK(all_physical(true));
    PB_CHECK(sim.cmd_sys_bt("enable").rfind("enabled ", 0) == 0);
    PB_CHECK(all_physical(false));
  } else {
    PB_CHECK(sim.cmd_sys_bt("enable").rfind("enabled ", 0) == 0);
    PB_CHECK(all_physical(false));
    PB_CHECK(sim.cmd_sys_bt("disable").rfind("disabled ", 0) == 0);
    PB_CHECK(all_physical(true));
  }
}

// ================ 覆盖率补测批（第 1 轮任务A · sim/control 未覆盖行）================
// 与上方“R4 真实路径”批互补：本批只做纯软件确定性测试（不依赖提权/物理硬件/
// 时序）。本轮任务给出的行号出自上一门禁覆盖所用的旧源版本（simulator.cpp
// SHA-256 12147881…，与 11:38 门禁二进制的 PDB 记录相符）；编写期间缺陷修复批于
// 15:19-15:21 落地源码（busid 白名单 / snoop 命令 / detach 按设备卸载），行号整体
// 平移、语义构造一一对应，测试行为不受影响。下列注释标注 新行号（旧行号）：
//   1) pipe_server.cpp WriteFile 失败分支 106,107（旧 100,101）—— 可确定性覆盖
//      （本节测试 1）。
//   2) simulator.cpp cmd_unload 吞 detach 抛错 270,271（旧 236,237）、find_usbip_exe
//      回退尾 339/342/344（旧 305/308/310）、cmd_attach 缺 usbip 抛错 444-445
//      （旧 316）—— 本机环境性不可达：usbip.exe 已装于固定候选路径
//      C:\Program Files\USBip\usbip.exe（本会话 ls 实证：1,514,256 字节）→
//      find_usbip_exe() 恒在 :338 提前返回。本节测试 2 带环境前置条件，在未装
//      usbip.exe 的机器上确定覆盖之；本机跳过。
//   3) simulator.cpp sys-bt 分支体 324-328（旧 290-293）—— 纯软件确定性路径仅
//      存在于“无物理射频”的机器（本节测试 3）；本机存在 Intel 射频
//      （USB\VID_8087&PID_0026，只读枚举实证）→ 本批跳过（上方真实路径批在提权
//      环境下覆盖 324-329）。成功 return 329（旧 294）纯软件恒不可达。
//   4) simulator.cpp cmd_unload_all 吞错 304,305（旧 270,271）、cmd_attach 收尾
//      （旧 342）与 commands.cpp execute 收尾 99,100（旧 96,97）—— 无条件不可覆盖
//      （结构性不可达 / 编译器生成的死机器码——已在 11:38 门禁二进制上以 PDB 行表
//      + 全 .text 反汇编实证；缺陷批改源后的新二进制需下一轮门禁复验，见
//      uncoverable 申报）。
// 约束“只在文件末尾追加”；<thread>/<atomic>/core/bt_radio.h 已由上方各节引入
//（重复包含有 #pragma once 保护，此处保持本节自包含再列一次）。
#include "core/bt_radio.h"

// —— pipe_server.cpp:106,107（旧 100,101）—— send_line 中 WriteFile 失败分支。
// 服务端线程：CreateNamedPipeA(512B 缓冲) → ConnectNamedPipe（阻塞，返回 ⇔ 本测试
// 客户端已连上：单实例管道 + 进程内唯一客户端）→ 立即 CloseHandle 且从不读一字节。
// 客户端 send_line 发送 4MiB 行，任何交错下 WriteFile 都不可能成功：
//   (a) CloseHandle 先于 WriteFile：对端已断 → 立即失败；
//   (b) WriteFile 先进入：4MiB ≫ 512B 管道缓冲且服务端永不读 → 阻塞，
//       CloseHandle 随即以 ERROR_BROKEN_PIPE 终止阻塞写。
// 两条路径都落入 :105 的 !WriteFile 真分支 → :106 CloseHandle → :107 return false
// （且在 :107 直接返回，到不了 peek 循环）。全程阻塞同步点（ConnectNamedPipe/
// WriteFile/CloseHandle），无 sleep、无时序假设。pipe_created/pipe_connected 断言
// 排除“客户端 40 次重试连不上、从 :100 返回”的假阳性（那不经过 :106/:107）：
// 管道折断发生在 pipe_connected 置位之后，若 WriteFile 因折断而失败则二者必为真。
PB_TEST(cov_sim_control_send_line_write_fails_broken_pipe) {
  const std::string name =
      "\\\\.\\pipe\\pb-cov-simctl-wfail-" + std::to_string(GetCurrentProcessId());
  HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  PB_CHECK(done != nullptr);
  std::atomic<bool> pipe_created{false};
  std::atomic<bool> pipe_connected{false};
  std::thread srv([&name, done, &pipe_created, &pipe_connected] {
    HANDLE pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 512,
                                   512, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;
    pipe_created.store(true);
    ConnectNamedPipe(pipe, nullptr);  // 返回 ⇔ 客户端 CreateFileA 已连接
    pipe_connected.store(true);
    CloseHandle(pipe);  // 折断管道；永不读一个字节
    SetEvent(done);
  });
  const std::string huge((size_t)4 << 20, 'x');  // 4 MiB ≫ 512 B 缓冲，写不可能完成
  std::vector<std::string> replies;
  PB_CHECK(!control::CommandClient::send_line(name, huge, replies, 5000));
  PB_CHECK(replies.empty());
  // WriteFile 失败机制成立的前提：管道确实创建、被本客户端连接后折断
  PB_CHECK(pipe_created.load());
  PB_CHECK(pipe_connected.load());
  WaitForSingleObject(done, 5000);
  srv.join();
  CloseHandle(done);
}

// —— simulator.cpp:270,271（旧 236,237，cmd_unload 吞掉 cmd_detach 的抛错）与
// :444-445（旧 316，cmd_attach 的 "usbip.exe not found" 抛出），连带 :339/:342/:344
//（旧 305/308/310，find_usbip_exe 回退尾）——
// 共同前置：两个固定候选路径（C:\Program Files\USBip\ 与 (x86)）都不存在。本机
// 已装（:338 恒命中）→ 本组行在本机不可达（见 uncoverable 申报）。本测试以
// find_usbip_exe()=="" 为前置：仅在未装 usbip.exe 的机器上执行体，届时上述各行
// 为确定结果；已装机器上直接 return（0 检查通过，不引入环境差异）。
// busid 用 "7-1"/"9-9"（合法 ^\d+-\d+$ 形态，valid_busid 白名单放行）。
PB_TEST(cov_sim_control_bare_machine_usbip_missing_paths) {
  if (!sim::Simulator::find_usbip_exe().empty()) return;  // 本机已装：上述行不可达
  sim::Simulator sim;
  PB_CHECK_EQ(sim.cmd_load_bt_adapter(), std::string("7-1"));
  // usbip.exe 缺失 → cmd_detach 在 :466 抛 "usbip.exe not found" →
  // cmd_unload 内 catch(...)（:268-271）吞掉 → 卸载照常完成
  PB_CHECK_EQ(sim.cmd_unload("7-1"), std::string("unloaded 7-1"));
  // cmd_attach：find_usbip_exe 走 :339 循环自然退出 + :342 SearchPathA 未命中 +
  // :344 返回空 → :444-445 确定抛 "usbip.exe not found…"
  bool threw = false;
  std::string msg;
  try {
    (void)sim.cmd_attach("9-9");
  } catch (const std::exception& e) {
    threw = true;
    msg = e.what();
  }
  PB_CHECK(threw);
  PB_CHECK(msg.find("usbip.exe not found") != std::string::npos);
}

// —— simulator.cpp:324-328（旧 290-293，sys-bt disable/enable 分支体）——
// 执行即调用 btradio::set_radios_enabled（bt_radio.cpp:72），它对每个物理射频发
// DIF_PROPERTYCHANGE / DICS_DISABLE / DICS_FLAG_GLOBAL（bt_radio.cpp:107-112）——
// 真实禁用/启用物理蓝牙硬件，破坏性、需管理员，纯软件单测禁止触碰。唯一软件
// 安全形态是本机无任何物理射频：set_radios_enabled 在 bt_radio.cpp:77-80 以
// "no physical bluetooth radio found" 早退（n=0，零状态变更）→ :324,:325,:326 执行、
// :327 以该确定文本抛错（execute 捕获 → 单行 "ERR …"）。前置检查 list_radios()
// 是只读 SetupDI 枚举（无副作用），且 set_radios_enabled 内部同样先 list_radios()，
// 同进程两次枚举结果一致，前置成立时体内行为确定。
// 本机存在 Intel 射频（USB\VID_8087&PID_0026）→ 前置不成立，本批跳过（真实
// 翻转已由上方 cov_sim_control_sys_bt_real_toggle_roundtrip 在提权环境覆盖）。
// :329（旧 :294，成功 return）在任何机器上都要求真的改动 ≥1 个射频才能到达，
// 纯软件恒不覆盖。
PB_TEST(cov_sim_control_sys_bt_disable_when_no_physical_radios) {
  bool has_physical = false;
  for (const auto& r : btradio::list_radios())
    if (r.physical) has_physical = true;
  if (has_physical) return;  // 本机有物理射频：执行体会真的禁用它，禁止
  sim::Simulator sim;
  auto out = control::execute(sim, "sys-bt disable");
  PB_CHECK_EQ(out.size(), 1u);
  PB_CHECK_EQ(out[0], std::string("ERR no physical bluetooth radio found"));
}

// ===================== 缺陷修复批（bug gui-port / bug detach-by-device）=====================
// （PB_TEST 名即 C 标识符，bug id 中的 '-' 映射为 '_'，同上 bug_attach_inject 批。）

// —— bug gui-port：内嵌 usbip 服务器端口来自 PB_PORT，默认 3425，非法回落默认 ——
// （usbip 客户端默认端口 3240 被本机另一项目常驻占用；tools/gui-verify.ps1 以
//   同一变量跟随。全程保存/还原 PB_PORT，不污染进程环境。）
PB_TEST(bug_gui_port_server_port_from_env) {
  char orig[16];
  DWORD orig_n = GetEnvironmentVariableA("PB_PORT", orig, sizeof(orig));
  auto restore = [&]() {
    if (orig_n > 0 && orig_n < sizeof(orig))
      SetEnvironmentVariableA("PB_PORT", orig);
    else
      SetEnvironmentVariableA("PB_PORT", nullptr);  // 进入前未设置 → 还原为未设置
  };
  SetEnvironmentVariableA("PB_PORT", nullptr);  // 未设置 → 默认 3425
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3425);
  SetEnvironmentVariableA("PB_PORT", "3499");  // 合法自定义值
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3499);
  SetEnvironmentVariableA("PB_PORT", "1");  // usbip -t 合法范围 1-65535 的两端
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 1);
  SetEnvironmentVariableA("PB_PORT", "65535");
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 65535);
  SetEnvironmentVariableA("PB_PORT", "abc");  // 非数字 → 回落默认
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3425);
  SetEnvironmentVariableA("PB_PORT", "3425x");  // 带尾巴 → 回落
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3425);
  SetEnvironmentVariableA("PB_PORT", "0");  // 0 不能作约定监听端口 → 回落
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3425);
  SetEnvironmentVariableA("PB_PORT", "65536");  // 越上界 → 回落
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3425);
  restore();
}

// —— bug gui-port：非 usbip 默认端口时 attach 命令必须带全局选项 -t ——
// （GUI 默认 3425 → 从 GUI 发起的 attach 恒带 -t；3240 是 usbip 客户端默认端口，
//   该端口下省略 -t 保持旧行为。另：新语法分支带 --receive-mode low-latency，
//   规避 usbip-win2 0.9.8 默认 zero-copy 接收模式的中断 IN 事件投递缺陷。）
PB_TEST(bug_gui_port_attach_carries_t_on_non_default_port) {
  const std::string exe = "C:\\Program Files\\USBip\\usbip.exe";
  auto cmds = sim::Simulator::attach_commands(exe, 3425, "7-1");
  PB_CHECK_EQ(cmds.size(), 2u);
  // usbip-win2 语法：-t 是全局选项，必须位于子命令 attach 之前
  PB_CHECK_EQ(cmds[0],
              std::string("\"C:\\Program Files\\USBip\\usbip.exe\" -t 3425 attach -r "
                          "127.0.0.1 -b 7-1 --receive-mode low-latency"));
  // 旧 usbip-win 语法：显式 --port（无该选项的旧版不识别，保持原样）
  PB_CHECK_EQ(cmds[1],
              std::string("\"C:\\Program Files\\USBip\\usbip.exe\" attach "
                          "--remote=127.0.0.1 --port=3425 --busid=7-1"));
  // usbip 默认端口 3240：-t 省略（保持旧行为），receive-mode 照带
  auto def = sim::Simulator::attach_commands(exe, 3240, "7-1");
  PB_CHECK_EQ(def[0],
              std::string("\"C:\\Program Files\\USBip\\usbip.exe\" attach -r 127.0.0.1 -b 7-1 "
                          "--receive-mode low-latency"));
}

// —— bug detach-by-device：解析 "usbip port" 输出建立 busid→端口映射 ——
// 样例 A 为本机 usbip-win2 0.9.8 真实输出（2026-11-01 会话实测，仅本机回环）；
// 样例 B 在 A 后追加第二个导入块并改用 CRLF 行尾，锚定多设备与 \r 容错。
PB_TEST(bug_detach_by_device_parse_imported_ports) {
  const std::string one =
      "Imported USB devices\n"
      "====================\n"
      "Port 01: device in use at High Speed(480Mbps)\n"
      "         OpenMoko, Inc. : unknown product (1d50:6222)\n"
      "           -> usbip://127.0.0.1:3240/1-1\n"
      "           -> remote bus/dev: 001/001\n"
      "           -> serial: \n"
      "           -> mode: zero-copy\n";
  auto m = sim::Simulator::parse_imported_ports(one);
  PB_CHECK_EQ(m.size(), 1u);
  PB_CHECK_EQ(m["1-1"], 1);

  const std::string two =
      "Imported USB devices\r\n"
      "====================\r\n"
      "Port 01: device in use at High Speed(480Mbps)\r\n"
      "         OpenMoko, Inc. : unknown product (1d50:6222)\r\n"
      "           -> usbip://127.0.0.1:3240/1-1\r\n"
      "           -> remote bus/dev: 001/001\r\n"
      "           -> serial: \r\n"
      "           -> mode: zero-copy\r\n"
      "Port 02: device in use at Full Speed(12Mbps)\r\n"
      "         (unknown vendor) : unknown product (1209:b007)\r\n"
      "           -> usbip://127.0.0.1:3425/7-2\r\n"
      "           -> remote bus/dev: 007/002\r\n"
      "           -> serial: \r\n"
      "           -> mode: zero-copy\r\n";
  auto m2 = sim::Simulator::parse_imported_ports(two);
  PB_CHECK_EQ(m2.size(), 2u);
  PB_CHECK_EQ(m2["1-1"], 1);  // 别的项目（3240 服务器）挂载的设备
  PB_CHECK_EQ(m2["7-2"], 2);  // 本项目（3425 服务器）挂载的设备
  // 只有表头（无导入块）→ 空映射
  PB_CHECK_EQ(sim::Simulator::parse_imported_ports("Imported USB devices\n====\n").size(), 0u);
}

// —— bug detach-by-device：按设备卸载 —— 目标 busid 只命中自己的 hub 口，绝不 --all ——
PB_TEST(bug_detach_by_device_detach_targets_only_matching_port) {
  const std::string two =
      "Imported USB devices\r\n"
      "====================\r\n"
      "Port 01: device in use at High Speed(480Mbps)\r\n"
      "           -> usbip://127.0.0.1:3240/1-1\r\n"
      "           -> mode: zero-copy\r\n"
      "Port 02: device in use at Full Speed(12Mbps)\r\n"
      "           -> usbip://127.0.0.1:3425/7-2\r\n"
      "           -> mode: zero-copy\r\n";
  const std::string exe = "C:\\Program Files\\USBip\\usbip.exe";
  // 卸载 7-2：只 detach -p 2（我们自己的口），且不含 --all
  std::string c = sim::Simulator::detach_command_for(exe, two, "7-2");
  PB_CHECK_EQ(c, std::string("\"C:\\Program Files\\USBip\\usbip.exe\" detach -p 2"));
  PB_CHECK(c.find("--all") == std::string::npos);
  // 别人挂载的 1-1：映射到口 1 —— 只有显式请求它时才会被摘，绝不被 --all 波及
  PB_CHECK_EQ(sim::Simulator::detach_command_for(exe, two, "1-1"),
              std::string("\"C:\\Program Files\\USBip\\usbip.exe\" detach -p 1"));
  // 未导入的 busid：空命令（无事可做；cmd_detach 据此返回 "not attached"，
  // 不再执行任何 detach）
  PB_CHECK_EQ(sim::Simulator::detach_command_for(exe, two, "9-9"), std::string(""));
  // 空输出（读不到 port 列表）同样无事可做
  PB_CHECK_EQ(sim::Simulator::detach_command_for(exe, "", "7-2"), std::string(""));
}

// ===================== 第 2 轮任务A 追加：parse_imported_ports 坏块头 =====================
// 纯内存解析（simulator.cpp:392-429），无 IO/子进程/网络，结果确定。
// 既有测试（bug_detach_by_device_parse_imported_ports 等）只喂合法 "Port 01/02/03:"
// 块头或无块头文本，:406-407（数字段含非数字字符）与 :411-412（数值 > 255）两处
// good=false; break; 从未执行。坏块头 ⇒ cur 保持 -1 ⇒ 该块内 "-> usbip://" 行
// 不得进入映射，且解析继续、后续合法块照常解析（坏块不污染状态）。
// detach 语义联动：坏块头块的 busid 视为未导入 → detach_command_for 空串
//（cmd_detach 返回 "not attached"，绝不 fallback 到 --all）。

// —— simulator.cpp:406,407 —— 块头数字段含非数字字符（"Port 1x:"）——
// 行走查：'P'0 'o'1 'r'2 't'3 ' '4 '1'5 'x'6 ':'7 → colon=7>5，i=5 '1' 合法压入
// v，i=6 'x' 非数字 → :406 good=false → :407 break（循环提前退出、:415 不采纳）。
PB_TEST(cov_sim_control_parse_ports_header_non_digit_rejected) {
  const std::string in =
      "Imported USB devices\n"
      "====================\n"
      "Port 1x: device in use at High Speed(480Mbps)\n"
      "           -> usbip://127.0.0.1:3240/1-1\n"
      "           -> mode: zero-copy\n"
      "Port 03: device in use at Full Speed(12Mbps)\n"
      "           -> usbip://127.0.0.1:3425/7-2\n";
  auto m = sim::Simulator::parse_imported_ports(in);
  PB_CHECK_EQ(m.size(), 1u);          // 坏块头的整块被丢弃，仅剩合法块
  PB_CHECK_EQ(m.count("1-1"), 0u);    // 坏块头之后的 usbip:// 行不进映射
  PB_CHECK_EQ(m.count("7-2"), 1u);    // 解析未受坏块影响，继续工作
  PB_CHECK_EQ(m["7-2"], 3);
  // 坏块头块的 busid → 视为未导入：空串（无事可做，绝不 --all 波及他人导入）
  PB_CHECK_EQ(sim::Simulator::detach_command_for("u.exe", in, "1-1"), std::string(""));
  // 小写 "port " 前缀走同一非数字路径（:398 第二个 rfind 命中 → :406-407）
  const std::string lower =
      "port xy: device in use\n"
      "           -> usbip://127.0.0.1:3240/2-1\n";
  PB_CHECK_EQ(sim::Simulator::parse_imported_ports(lower).size(), 0u);
}

// —— simulator.cpp:411,412 —— 块头口号数值 > 255（usbip detach -p 合法 1-255）——
// "Port 999:" → i=5 v=9、i=6 v=99、i=7 v=999>255 → :411 good=false → :412 break。
// 边界锚定：255（最大合法口号）接受、256 即拒绝，防止“整段非数字”之外的第二
// 种坏形态被误放行。
PB_TEST(cov_sim_control_parse_ports_header_over_255_rejected) {
  const std::string in =
      "Port 999: device in use at High Speed(480Mbps)\n"
      "           -> usbip://127.0.0.1:3240/1-1\n"
      "Port 02: device in use at Full Speed(12Mbps)\n"
      "           -> usbip://127.0.0.1:3425/7-2\n";
  auto m = sim::Simulator::parse_imported_ports(in);
  PB_CHECK_EQ(m.size(), 1u);         // 越界块头的整块被丢弃
  PB_CHECK_EQ(m.count("1-1"), 0u);
  PB_CHECK_EQ(m["7-2"], 2);          // 合法块照常解析
  const std::string b255 =
      "Port 255: device in use\n"
      "           -> usbip://127.0.0.1:3425/9-1\n";
  auto m255 = sim::Simulator::parse_imported_ports(b255);
  PB_CHECK_EQ(m255.size(), 1u);      // 255 不越界（>255 才拒）
  PB_CHECK_EQ(m255["9-1"], 255);
  const std::string b256 =
      "Port 256: device in use\n"
      "           -> usbip://127.0.0.1:3425/9-2\n";
  PB_CHECK_EQ(sim::Simulator::parse_imported_ports(b256).size(), 0u);  // 256 即拒
}

// ===================== 第 3 轮任务B 追加：控制命令分发层（execute）真实成功路径 ====
// 盘点矩阵 3 行（attach / detach / sys-bt 真实路径）的既有覆盖绕过了 commands.cpp
// 的分发层或只在 exe 级断言：
//   - execute("attach …") 的成功行 "OK attached <busid>"（commands.cpp:78）没有单测
//     钉死 —— test_coverage_extra.cpp:57-58 对 OK/ERR 二选一放行，R4 批直调
//     sim.cmd_attach、check-tools H1 在 exe 级（不进单测覆盖统计）；
//   - execute("detach …") 的成功行 "OK detached"（commands.cpp:81）同样只是三选一
//     放行（cov_sim_control_detach_via_execute）；
//   - cmd_unload 内嵌的 best-effort cmd_detach（simulator.cpp:268-272）只覆盖过
//     “usbip.exe 缺失抛错被吞”形态（cov_sim_control_bare_machine_usbip_missing_paths），
//     “真有内核导入时 unload 把它真解绑”从未自动化。
// 复用上方 R4 批辅助（cov_r4_elevated / cov_r4_usbip / cov_r4_import_count /
// cov_r4_import_port_of，同 TU 匿名命名空间，定义先于本节）。前置缺失（无
// usbip.exe / 未提权 = CI 与裸机）按既有惯例提前 return（0 检查通过）。

// —— 矩阵行 1+2：attach / detach 经控制命令分发层的真实成功行 ——
// 与 cov_sim_control_attach_real_kernel_loopback 的差别：走 control::execute
//（CLI `cmd attach` / GUI 按钮同一入口），钉死 commands.cpp:78/:81 的 OK 拼接行；
// 判据仍是真 usbip.exe + 内核 vhci（marker = usbip://127.0.0.1:<port>/7-1）。
PB_TEST(cov_r3b_execute_attach_detach_real_ok_lines) {
  std::string exe = sim::Simulator::find_usbip_exe();
  if (exe.empty() || !cov_r4_elevated()) return;  // CI/裸机/未提权：真实路径不可达
  if (!cov_r4_vhci_ready(exe)) return;  // vhci 驱动缺失：OK 成功行不可达，跳过
  net::WsaSession wsa;
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));  // 随机回环端口（避开常驻 3240 的其他 bench）
  PB_CHECK_EQ(control::execute(sim, "load-hid profiles/gamepad.json")[0], std::string("OK 7-1"));
  const std::string marker = "usbip://127.0.0.1:" + std::to_string(sim.port()) + "/7-1";
  bool attached = false;
  auto ra = control::execute(sim, "attach 7-1");  // 控制命令层 → 真 usbip.exe + 内核导入
  PB_CHECK_EQ(ra.size(), 1u);
  if (ra.size() == 1u && ra[0] == "OK attached 7-1") attached = true;
  PB_CHECK(attached);  // commands.cpp:78 成功行（此前无单测钉死）
  if (attached) {
    PB_CHECK(cov_r4_usbip(exe, "port").find(marker) != std::string::npos);  // 内核侧确实导入
    auto rd = control::execute(sim, "detach 7-1");  // 控制命令层 detach 成功分支
    PB_CHECK_EQ(rd.size(), 1u);
    PB_CHECK_EQ(rd[0], std::string("OK detached"));  // commands.cpp:81 成功行（不再三选一）
    std::string after = cov_r4_usbip(exe, "port");
    PB_CHECK(after.find(marker) == std::string::npos);  // 真解绑
    if (after.find(marker) != std::string::npos) {      // 断言已记失败：兜底只摘自己的口
      int p = cov_r4_import_port_of(after, marker);
      if (p >= 1) cov_r4_usbip(exe, "detach -p " + std::to_string(p));
    }
  } else {
    // attach 未成功但内核可能仍留下半个导入 —— 兜底清理（绝不 --all）
    int p = cov_r4_import_port_of(cov_r4_usbip(exe, "port"), marker);
    if (p >= 1) cov_r4_usbip(exe, "detach -p " + std::to_string(p));
  }
  sim.stop();
}

// —— 矩阵行 2 延伸：unload 对真实内核导入的 best-effort 解绑 ——
// cmd_unload 先 cmd_detach 再摘模型（simulator.cpp:268-272）。真挂载自己的 7-1 后
// unload 必须让内核导入消失、他人导入原样保留（绝不 --all 波及；本机可能同时挂着
// 其他 usbip bench 的导入），且模型确已卸下（再 unload → ERR unknown busid）。
PB_TEST(cov_r3b_unload_unbinds_real_kernel_import) {
  std::string exe = sim::Simulator::find_usbip_exe();
  if (exe.empty() || !cov_r4_elevated()) return;  // CI/裸机/未提权：真实路径不可达
  if (!cov_r4_vhci_ready(exe)) return;  // vhci 驱动缺失：真实导入不可达，跳过
  const int foreign = cov_r4_import_count(cov_r4_usbip(exe, "port"));  // 他人既有导入
  net::WsaSession wsa;
  sim::Simulator sim;
  std::string err;
  PB_CHECK(sim.start(&err, 0));
  PB_CHECK_EQ(control::execute(sim, "load-hid profiles/gamepad.json")[0], std::string("OK 7-1"));
  const std::string marker = "usbip://127.0.0.1:" + std::to_string(sim.port()) + "/7-1";
  bool attached = false;
  auto ra = control::execute(sim, "attach 7-1");
  if (ra.size() == 1u && ra[0] == "OK attached 7-1") attached = true;
  PB_CHECK(attached);
  if (attached) {
    PB_CHECK_EQ(control::execute(sim, "unload 7-1")[0], std::string("OK unloaded 7-1"));
    std::string after = cov_r4_usbip(exe, "port");
    PB_CHECK(after.find(marker) == std::string::npos);  // unload 内部的 detach 真解绑
    PB_CHECK_EQ(cov_r4_import_count(after), foreign);   // 他人导入原样保留
    PB_CHECK_EQ(control::execute(sim, "unload 7-1")[0],
                std::string("ERR unknown busid 7-1"));  // 模型确已卸下
    if (after.find(marker) != std::string::npos) {      // 断言已记失败：兜底清理
      int p = cov_r4_import_port_of(after, marker);
      if (p >= 1) cov_r4_usbip(exe, "detach -p " + std::to_string(p));
    }
  } else {
    int p = cov_r4_import_port_of(cov_r4_usbip(exe, "port"), marker);
    if (p >= 1) cov_r4_usbip(exe, "detach -p " + std::to_string(p));
  }
  sim.stop();
}
