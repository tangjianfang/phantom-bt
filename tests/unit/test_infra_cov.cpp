// 覆盖率补充（infra）：测试框架自身（test_framework.h）与 core/test_runner.cpp
// 的未覆盖行。被测源码只读；本文件只新增 cov_infra_* 前缀的用例。
#include "test/test_framework.h"

#include <windows.h>

#include "core/test_runner.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace pb;

// —— test_framework.h: TestCtx::fail 失败路径（全套件全绿时从未执行）——
// 对局部 ctx 直接调用 fail()：只影响局部计数，不会让外层套件变红。
PB_TEST(cov_infra_testctx_fail_counts) {
  TestCtx ctx;
  ctx.name = "cov_infra_local_ctx";
  PB_CHECK_EQ(ctx.checks, 0);
  PB_CHECK_EQ(ctx.failures, 0);
  ctx.fail("injected failure", __FILE__, __LINE__);
  PB_CHECK_EQ(ctx.failures, 1);
  ctx.fail("injected failure 2", __FILE__, __LINE__);
  PB_CHECK_EQ(ctx.failures, 2);
}

// —— run_all 的异常捕获（catch std::exception / catch ...）与失败汇总分支 ——
// 受开关控制的“会抛异常”的用例：真实套件（main 的外层 run_all，无过滤器）
// 运行时开关为 0、安静通过，不影响整份套件的绿/红；只有下面的
// cov_infra_run_all_* 用子串过滤器单独驱动它们时才抛，从而覆盖 run_all 内部
// 的两个 catch 与 "N check(s) FAILED" 分支。run_all 自身捕获一切异常，
// 内层调用不会把异常漏到外层。对 tests_ 向量的重入只读迭代是安全的
// （无注册增删）。
namespace {
int cov_infra_throw_mode = 0;  // 0=正常通过 1=抛 std::exception 2=抛非 std 异常
struct CovInfraThrowGuard {    // 任何退出路径都把开关复位为 0
  ~CovInfraThrowGuard() { cov_infra_throw_mode = 0; }
};
}  // namespace

PB_TEST(cov_infra_helper_throws_std) {
  if (cov_infra_throw_mode == 1) throw std::runtime_error("injected std failure");
  PB_CHECK(true);
}

PB_TEST(cov_infra_helper_throws_unknown) {
  if (cov_infra_throw_mode == 2) throw 42;  // 非 std::exception → catch (...)
  PB_CHECK(true);
}

PB_TEST(cov_infra_run_all_catches_and_counts_failures) {
  CovInfraThrowGuard guard;
  TestRegistry& reg = TestRegistry::instance();
  // 过滤器只匹配对应那一个用例（子串互不包含），内层只跑它、只失败它。
  // 抛 std::exception → catch (const std::exception&) → ctx.fail(e.what())
  cov_infra_throw_mode = 1;
  PB_CHECK_EQ(reg.run_all("cov_infra_helper_throws_std"), 1);
  // 抛非 std 异常 → catch (...) → ctx.fail("unknown exception")
  cov_infra_throw_mode = 2;
  PB_CHECK_EQ(reg.run_all("cov_infra_helper_throws_unknown"), 1);
  // 开关关闭时同一用例正常通过并返回 0：证明外层无过滤套件里它是安静的
  cov_infra_throw_mode = 0;
  PB_CHECK_EQ(reg.run_all("cov_infra_helper_throws_std"), 0);
}

// —— run_capture 的“子进程已退出、孙进程静默持有写端”的排空完成分支 ——
// cmd 立即退出，孙进程 ping 继承管道写端（start "" /b 不开新窗口、继承句柄，
// 与既有 tr_run_capture_stop_during_drain 同一机制）；ping 两次输出之间静默
// 约 1s，远大于 ~300ms（6×50ms）的排空静默阈值 → 命中 quiet_polls>=6 的
// flush+break，而不是 pipe_gone（写端仍被 ping 持有，PeekNamedPipe 不失败）。
// 该路径非超时：返回值是已退出的 cmd 的退出码 0（既非 -1 启动失败，也非 -2）。
// ping 的输出是否赶在 break 前到达是时序相关的，因此不对 lines 内容断言。
PB_TEST(cov_infra_run_capture_drain_completes) {
  std::vector<std::string> lines;
  ULONGLONG t0 = GetTickCount64();
  int rc = testrunner::run_capture(
      "cmd.exe /c start \"\" /b ping.exe -n 5 127.0.0.1",
      [&](const std::string& l) { lines.push_back(l); });
  ULONGLONG dt = GetTickCount64() - t0;
  PB_CHECK_EQ(rc, 0);
  PB_CHECK(dt < 20000);  // 排空完成即返回（或 ping 自然结束），绝不是超时路径
}

// ============== 追加（第 2 轮）：find_tests_exe 全候选 miss（test_runner.cpp:42-43） ==============

#include <utility>  // std::pair
#include <wchar.h>  // _wcsicmp

#include "core/env_check.h"

namespace {
// 规范解析（GetFullPathNameW 展开 "..\"），供大小写不敏感的路径比较。
std::wstring cov_infra_canon(const std::wstring& p) {
  wchar_t out[1024];
  DWORD n = GetFullPathNameW(p.c_str(), 1024, out, nullptr);
  if (n == 0 || n >= 1024) return p;  // 解析失败仅影响比较，不影响覆盖
  return std::wstring(out, n);
}

// RAII：任何退出路径都把已改名的候选文件改回原名，恢复机器状态。
struct CovInfraRenameBack {
  std::vector<std::pair<std::wstring, std::wstring>> done;  // {原名, 临时名}
  ~CovInfraRenameBack() {
    for (auto& r : done) MoveFileW(r.second.c_str(), r.first.c_str());
  }
};
}  // namespace

// test_runner.cpp:42-43 —— 三个候选全部 miss：循环推进（:42）并落到 return ""
// （:43）。单测进程本身就是 build/<cfg>/tests/phantombt-tests.exe：调试布局
// （覆盖门禁所用 build\debug，LastCoverageResults.log 实证）下 ..\tests\ /
// 同目录 / ..\..\debug\tests\ 三个候选规范解析到同一路径，即本进程的 exe；
// 发布布局下前两个候选是本进程，第三个解析到 build\debug 树里的同名 exe
// （在场时一并临时改名）。Windows 加载器以 FILE_SHARE_READ|FILE_SHARE_DELETE
// 打开映像，运行中的 exe 可被同目录 rename（本会话实测：临时目录中运行中的
// ping 副本 rename 成功）；exe_dir() 只取 GetModuleFileNameW 的目录部分
// （env_check.cpp:90-98），同目录改名不改变它。于是把现存候选全部改名 →
// find_tests_exe() 的三次 file_exists 全 miss → 覆盖 42/43；析构守卫随即把
// 名字改回。与 test_core_cov.cpp:78-107（exe 旁临时创建/删除 winrt-probe.exe）
// 同属对 exe 目录的可逆纯软件操作，无 sleep、无时序依赖。
PB_TEST(cov_infra_find_tests_exe_all_candidates_miss) {
  std::wstring dir = pb::env::exe_dir();
  PB_CHECK(!dir.empty());
  wchar_t selfbuf[1024];
  DWORD n = GetModuleFileNameW(nullptr, selfbuf, 1024);
  PB_CHECK(n > 0 && n < 1024);
  std::wstring self = cov_infra_canon(std::wstring(selfbuf, n));

  // 与 src/core/test_runner.cpp:34-38 完全相同的候选组装，收集现存的候选（去重）。
  const wchar_t* kCandidates[] = {
      L"..\\tests\\phantombt-tests.exe",
      L"phantombt-tests.exe",
      L"..\\..\\debug\\tests\\phantombt-tests.exe",
  };
  std::vector<std::wstring> hits;
  for (auto* c : kCandidates) {
    std::wstring full = cov_infra_canon(dir + c);
    if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    bool dup = false;
    for (auto& h : hits)
      if (_wcsicmp(h.c_str(), full.c_str()) == 0) dup = true;
    if (!dup) hits.push_back(full);
  }
  bool self_is_candidate = false;
  for (auto& h : hits)
    if (_wcsicmp(h.c_str(), self.c_str()) == 0) self_is_candidate = true;
  PB_CHECK(self_is_candidate);  // 同目录候选就是运行中的本进程 exe，必然在场
  PB_CHECK(!testrunner::find_tests_exe().empty());  // 改名前必命中（:41）

  CovInfraRenameBack guard;
  bool self_renamed = false, all_renamed = true;
  for (auto& h : hits) {
    std::wstring tmp = h + L".cov_infra_tmp";
    if (GetFileAttributesW(tmp.c_str()) != INVALID_FILE_ATTRIBUTES ||
        !MoveFileW(h.c_str(), tmp.c_str())) {
      all_renamed = false;  // 残留临时名或外来候选被并发占用：跳过该文件
      continue;
    }
    guard.done.push_back({h, tmp});
    if (_wcsicmp(h.c_str(), self.c_str()) == 0) self_renamed = true;
  }
  // 本进程 exe 的改名必须成功——运行中 exe 的同目录 rename 是加载器允许的
  // 操作；失败说明环境异常，宁可让测试变红也不要静默丢覆盖。
  PB_CHECK(self_renamed);
  // 全部现存候选已改名 → 三次 file_exists 全 miss：循环推进（:42）+ return ""（:43）
  if (all_renamed) PB_CHECK_EQ(testrunner::find_tests_exe(), "");
}  // 守卫析构：全部候选改回原名

// ============== 追加（第 3 轮）：winrt-probe 探针模式契约（进程级） ==============
// 盘点缺口：src/probes 此前无任何单测（覆盖率口径排除 probes；ci.yml 头部注释
// 明示真机段不进 CI），四个模式只有 run-m*.ps1 真机腿调用、缺环境自动 SKIP。
// 此处对每个模式中"无硬件也成立"的部分做进程级契约测试：
//   - 参数/模式错误路径（rc=3 + stderr 提示，main.cpp:283-296）；
//   - ble 模式 12 位十六进制地址前置校验（main.cpp:235，在任何 WinRT 设备
//     调用之前，确定性拒绝）；
//   - radio-registry 纯注册表降级口径（main.cpp:166-187，无 WinRT 依赖）；
//   - 各模式"恰好一行 {"result":...} JSON + rc∈{0,2}"的输出契约
//     （main.cpp:71-87 finish() 的统一约定），对 radio/hid/watcher/ble 冒烟。
// 需要真实硬件/驱动的部分（hid 收到注入报告、radio 出现虚拟无线电、watcher
// 收到 PhantomKb、ble 完成 GATT+HID 节点发现）仍只能在真机腿验证。
// 断言的 rc/输出取值均与本机 2026-11-01 对预构建 build/release/src/
// winrt-probe.exe 的逐模式实跑结果一致（无参数/未知模式 rc=3；ble 坏地址、
// radio-registry、radio、hid、watcher、ble 好地址各产恰好一行 JSON）。
// 探针定位：本进程在 build/<cfg>/tests/，同构建树探针在
// build/<cfg>/src/winrt-probe.exe（与 test_runner.cpp:34-38 的布局推导同源）。

#include "core/bytes.h"  // pb::from_w

namespace {
// build/<cfg>/tests/../src/winrt-probe.exe；不在场返回空串（此时测试显式变红：
// 正常 preset 构建总是产出该目标）。
std::string cov_infra_probe_exe() {
  std::wstring dir = pb::env::exe_dir();
  if (dir.empty()) return "";
  std::wstring probe = dir + L"..\\src\\winrt-probe.exe";
  if (GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES) return "";
  return pb::from_w(probe);
}

// 运行 "winrt-probe <args>"，收集合流输出行（run_capture 把 stdout 与 stderr
// 接到同一管道：test_runner.cpp 的 STARTUPINFOW 双句柄），返回子进程退出码。
int cov_infra_run_probe(const std::string& args, std::vector<std::string>& lines,
                        DWORD timeout_ms = 60000) {
  std::string exe = cov_infra_probe_exe();
  if (exe.empty()) return -1;
  return testrunner::run_capture("\"" + exe + "\" " + args,
                                 [&](const std::string& l) { lines.push_back(l); },
                                 nullptr, timeout_ms);
}

// finish() 输出契约：恰好一行以 {"result": 开头的 JSON（main.cpp:71-87）。
std::vector<std::string> cov_infra_json_lines(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  for (const std::string& l : lines)
    if (l.rfind("{\"result\":", 0) == 0) out.push_back(l);
  return out;
}
}  // namespace

// main.cpp:283-286 / 295-296 —— 无参数与未知模式：rc=3 + 提示（stderr 合流可见）。
PB_TEST(cov_infra_probe_mode_dispatch_errors) {
  std::vector<std::string> lines;
  PB_CHECK_EQ(cov_infra_run_probe("", lines), 3);
  bool usage = false;
  for (const std::string& l : lines)
    if (l.find("usage:") != std::string::npos) usage = true;
  PB_CHECK(usage);

  lines.clear();
  PB_CHECK_EQ(cov_infra_run_probe("no-such-mode", lines), 3);
  bool not_impl = false;
  for (const std::string& l : lines)
    if (l.find("not implemented") != std::string::npos) not_impl = true;
  PB_CHECK(not_impl);
}

// main.cpp:235 —— ble 参数校验：非 12 位十六进制地址在触碰 WinRT 前即拒绝。
// 确定性行为（本机实测 rc=2 + {"result":"fail","reason":"bad address"}）。
PB_TEST(cov_infra_probe_ble_bad_address_rejected) {
  std::vector<std::string> lines;
  PB_CHECK_EQ(cov_infra_run_probe("ble --address ZZZ", lines), 2);
  auto js = cov_infra_json_lines(lines);
  PB_CHECK_EQ(js.size(), (size_t)1);
  if (js.size() == 1) {
    PB_CHECK(js[0].find("\"result\":\"fail\"") != std::string::npos);
    PB_CHECK(js[0].find("bad address") != std::string::npos);
  }
}

// main.cpp:166-187 —— radio-registry 降级口径：纯注册表扫描（HKLM USB 枚举），
// 无 WinRT/硬件/admin 依赖，任何 Windows 都可跑。绑定过设备 → rc=0 +
// "driver-bound:..."；未绑定 → rc=2（本机今日实测 nodev）。两分支都断言。
PB_TEST(cov_infra_probe_radio_registry_contract) {
  std::vector<std::string> lines;
  int rc = cov_infra_run_probe("radio-registry", lines);
  PB_CHECK(rc == 0 || rc == 2);
  auto js = cov_infra_json_lines(lines);
  PB_CHECK_EQ(js.size(), (size_t)1);
  if (js.size() == 1) {
    if (rc == 0) {
      PB_CHECK(js[0].find("\"result\":\"pass\"") != std::string::npos);
      PB_CHECK(js[0].find("driver-bound:") != std::string::npos);
    } else {
      PB_CHECK(js[0].find("\"result\":\"fail\"") != std::string::npos);
    }
  }
}

// 以下四个冒烟用例共享的输出契约：finish() 只产 rc 0/2 与恰好一行 result JSON；
// 真实硬件是否存在只影响 0 还是 2，不影响契约本身（无蓝牙环境如 CI runner 上
// 四模式均走 fail 分支：noradio/nodev/noseen 等，本机 2026-11-01 实测同值）。
// main.cpp:148-163 radio（WinRT GetRadiosAsync）：不挂死、按契约报告。
PB_TEST(cov_infra_probe_radio_winrt_json_contract) {
  std::vector<std::string> lines;
  int rc = cov_infra_run_probe("radio", lines);
  PB_CHECK(rc == 0 || rc == 2);
  auto js = cov_infra_json_lines(lines);
  PB_CHECK_EQ(js.size(), (size_t)1);
  if (js.size() == 1) PB_CHECK(js[0].find("\"result\":\"") != std::string::npos);
}

// main.cpp:89-146 hid：--wait-ms 1 把发现循环压到一轮；无设备环境确定性
// 走 "nodev" fail 分支（本机实测 0.16s）。
PB_TEST(cov_infra_probe_hid_json_contract) {
  std::vector<std::string> lines;
  int rc = cov_infra_run_probe(
      "hid --vid 0x1209 --pid 0xB007 --usage-page 0xFF00 --usage 0x01 --wait-ms 1", lines);
  PB_CHECK(rc == 0 || rc == 2);
  auto js = cov_infra_json_lines(lines);
  PB_CHECK_EQ(js.size(), (size_t)1);
  if (js.size() == 1) PB_CHECK(js[0].find("\"result\":\"") != std::string::npos);
}

// main.cpp:189-229 watcher：--duration-ms 300 限定监听窗口；无广播/无蓝牙
// 均落 fail 分支（noseen/hresult，本机实测 0.39s），有广播则 pass——契约不变。
PB_TEST(cov_infra_probe_watcher_json_contract) {
  std::vector<std::string> lines;
  int rc = cov_infra_run_probe("watcher --duration-ms 300", lines);
  PB_CHECK(rc == 0 || rc == 2);
  auto js = cov_infra_json_lines(lines);
  PB_CHECK_EQ(js.size(), (size_t)1);
  if (js.size() == 1) PB_CHECK(js[0].find("\"result\":\"") != std::string::npos);
}

// main.cpp:231-278 ble：合法 12 位地址（run-m3.ps1:37 同值，字节序与
// profiles/ble_keyboard.json 的 C0:11:22:33:44:55 对应）经 FromBluetoothAddressAsync
// 报告——无设备时 "nodev" fail（本机实测 0.08s），不挂死、不崩溃。
PB_TEST(cov_infra_probe_ble_valid_addr_json_contract) {
  std::vector<std::string> lines;
  int rc = cov_infra_run_probe("ble --address 5544332211C0 --wait-ms 200", lines, 90000);
  PB_CHECK(rc == 0 || rc == 2);
  auto js = cov_infra_json_lines(lines);
  PB_CHECK_EQ(js.size(), (size_t)1);
  if (js.size() == 1) PB_CHECK(js[0].find("\"result\":\"") != std::string::npos);
}

// ============== 追加（第 2 轮任务B）：多设备并发导入（回环）+ 单导出单导入守卫 ==============
// 盘点行「多设备并发 attach（真机）」注明"回环与真机均未测"，且回顾缺陷 #10
// （同一设备二次 import 无守卫、sink 被覆盖，src/usbip/server.cpp）已在当前树修复
// （server.cpp:33-37 imported_busids 登记 + :200-208 拒绝二次导入 + :245-249 连接
// 清理时释放），但该修复此前无任何测试锚定。真机半边（usbip.exe 双 attach）由
// tools/check-tools.ps1 的 H4 节覆盖；此处覆盖回环半边：
//   1) 两条连接并发 import 两个不同设备 → 均成功，且各自的控制传输走各自的
//      sink（描述符 vid 不同：vendor HID 0x1209 vs 蓝牙适配器 0x0B00）；
//   2) 第二条连接 import 已被占用的 busid → OP_REP_IMPORT status=1（守卫），
//      且被拒连接仍留在控制循环（DEVLIST 照常应答）；
//   3) 占用连接断开后 busid 被释放 → 同一连接重试 import 成功、sink 正常服务。
// 全部进程内（Server + TcpConn，无内核依赖），与 test_usbip_server.cpp 同一模式。

#include "usbip/server.h"

#include "bluetooth/adapter_model.h"
#include "core/net.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"
#include "sim/device_registry.h"

namespace {
std::vector<uint8_t> cov_infra_usbip_recv_n(net::TcpConn& c, size_t n) {
  std::vector<uint8_t> v(n);
  std::string err;
  if (!c.recv_exact(v.data(), n, &err)) throw std::runtime_error("cov_infra recv_n: " + err);
  return v;
}

// OP_REQ_IMPORT（8B op_common + 32B busid），返回应答 status 字节（0=ok 1=err）。
// 成功时调用方需自行再读 312B 导出描述符（与 test_usbip_server.cpp import_7_1 同帧型）。
int cov_infra_usbip_import(net::TcpConn& c, const char* busid) {
  ByteWriter w;
  w.u16be(usbip::OP_VERSION);
  w.u16be(usbip::OP_REQ_IMPORT);
  w.u32be(0);
  char b[32] = {};
  for (size_t i = 0; busid[i] && i < 31; ++i) b[i] = busid[i];
  w.bytes((const uint8_t*)b, 32);
  std::string err;
  if (!c.send_all(w.buf().data(), w.buf().size(), &err))
    throw std::runtime_error("cov_infra import send: " + err);
  auto head = cov_infra_usbip_recv_n(c, 8);
  if (head[3] != 0x03) throw std::runtime_error("cov_infra import: not REP_IMPORT");
  return head[7];
}

void cov_infra_usbip_send_cmd(net::TcpConn& c, uint32_t seq, uint32_t devid, uint32_t ep,
                              uint32_t dir, int32_t len, const uint8_t setup[8]) {
  ByteWriter w;
  w.u32be(usbip::USBIP_CMD_SUBMIT);
  w.u32be(seq);
  w.u32be(devid);
  w.u32be(dir);
  w.u32be(ep);
  w.u32be(0);
  w.u32be((uint32_t)len);
  w.u32be(0);
  w.u32be(0);
  w.u32be(0);
  w.bytes(setup, 8);
  std::string err;
  if (!c.send_all(w.buf().data(), w.buf().size(), &err))
    throw std::runtime_error("cov_infra send_cmd: " + err);
}

std::shared_ptr<hid::HidModel> cov_infra_add_vendor_device(sim::DeviceRegistry& reg) {
  auto prof = hid::HidProfile::from_json(json::parse(
      "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\",\"product\":\"v\"}"));
  auto m = std::make_shared<hid::HidModel>(prof, 7, 1);
  reg.add(m);
  return m;
}
}  // namespace

// 两条连接并发导入两个不同设备（vendor HID 7-1 + 虚拟蓝牙适配器 7-2）：均成功、
// 各自 sink 各自服务——GET_DESCRIPTOR(device) 的 idVendor 必须分别是 0x1209 与
// 0x0B00（adapter_model.cpp:18），证明并发导入后设备模型与连接一一对应。
PB_TEST(cov_infra_two_devices_concurrent_import) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  cov_infra_add_vendor_device(reg);
  auto bt = std::make_shared<bt::AdapterModel>(pb::hci::ControllerConfig{}, 7, 2);
  PB_CHECK_EQ(reg.add(bt), std::string("7-2"));

  usbip::Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c1 = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c1.is_open());
  auto c2 = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c2.is_open());

  PB_CHECK_EQ(cov_infra_usbip_import(c1, "7-1"), 0);
  (void)cov_infra_usbip_recv_n(c1, 312);  // 导出描述符
  PB_CHECK_EQ(cov_infra_usbip_import(c2, "7-2"), 0);
  (void)cov_infra_usbip_recv_n(c2, 312);

  uint8_t getdev[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  cov_infra_usbip_send_cmd(c1, 1, 0x00070001, 0, usbip::DIR_IN, 18, getdev);
  cov_infra_usbip_send_cmd(c2, 2, 0x00070002, 0, usbip::DIR_IN, 18, getdev);
  auto r1 = cov_infra_usbip_recv_n(c1, usbip::RET_SUBMIT_HDR_SIZE + 18);
  auto r2 = cov_infra_usbip_recv_n(c2, usbip::RET_SUBMIT_HDR_SIZE + 18);
  PB_CHECK_EQ(r1[3], 0x03);  // RET_SUBMIT
  PB_CHECK_EQ(r2[3], 0x03);
  size_t hd = usbip::RET_SUBMIT_HDR_SIZE;
  PB_CHECK_EQ((int)r1[hd], 18);       // bLength
  PB_CHECK_EQ((int)r1[hd + 1], 1);    // DT_DEVICE
  PB_CHECK_EQ((int)r2[hd], 18);
  PB_CHECK_EQ((int)r2[hd + 1], 1);
  int vid1 = r1[hd + 8] | (r1[hd + 9] << 8);  // idVendor LE
  int vid2 = r2[hd + 8] | (r2[hd + 9] << 8);
  PB_CHECK_EQ(vid1, 0x1209);  // vendor HID
  PB_CHECK_EQ(vid2, 0x0B00);  // 蓝牙适配器
  c1.close();
  c2.close();
  srv.stop();
}

// 单导出单导入守卫（server.cpp:200-208）：第二连接 import 已占用 busid → status=1
// 且不覆盖首连接的 sink；被拒连接留在控制循环（DEVLIST 仍应答）；占用连接断开后
// 占用释放（:245-249）→ 同一连接重试 import 成功并可正常做控制传输。
PB_TEST(cov_infra_same_busid_reimport_guard_and_release) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  cov_infra_add_vendor_device(reg);
  usbip::Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto ca = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(ca.is_open());
  auto cb = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(cb.is_open());

  PB_CHECK_EQ(cov_infra_usbip_import(ca, "7-1"), 0);
  (void)cov_infra_usbip_recv_n(ca, 312);

  // 守卫：二次导入必须被拒（回归到"覆盖 sink"旧缺陷时此处变红）
  PB_CHECK_EQ(cov_infra_usbip_import(cb, "7-1"), 1);

  // 被拒后连接仍在控制循环：DEVLIST 照常应答（1 设备 + 1 HID 接口）
  ByteWriter wd;
  wd.u16be(usbip::OP_VERSION);
  wd.u16be(usbip::OP_REQ_DEVLIST);
  wd.u32be(0);
  PB_CHECK(cb.send_all(wd.buf().data(), wd.buf().size(), &err));
  auto dh = cov_infra_usbip_recv_n(cb, 12);
  PB_CHECK_EQ((int)dh[3], 0x05);  // OP_REP_DEVLIST
  PB_CHECK_EQ((int)dh[11], 1);    // ndev
  (void)cov_infra_usbip_recv_n(cb, 312 + 4);

  // 占用连接断开 → 占用释放 → 重试导入成功（服务器连接线程清理为异步，轮询等待）
  ca.close();
  int st = 1;
  for (int i = 0; i < 60 && st != 0; ++i) {
    st = cov_infra_usbip_import(cb, "7-1");
    if (st != 0) Sleep(50);
  }
  PB_CHECK_EQ(st, 0);
  (void)cov_infra_usbip_recv_n(cb, 312);

  // 重导入后 sink 生效：控制传输有 RET_SUBMIT 应答
  uint8_t getdev[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  cov_infra_usbip_send_cmd(cb, 7, 0x00070001, 0, usbip::DIR_IN, 18, getdev);
  auto rb = cov_infra_usbip_recv_n(cb, usbip::RET_SUBMIT_HDR_SIZE + 18);
  PB_CHECK_EQ(rb[3], 0x03);
  PB_CHECK_EQ((int)rb[usbip::RET_SUBMIT_HDR_SIZE], 18);

  cb.close();
  srv.stop();
}

// ============ 第 3 轮任务B 追加：SMP c1/s1 规范工作样例（外部正确性锚点） ============
// 盘点行"M3 真机：SMP Just Works 配对（c1 排布/confirm 校验）裁决"。
// 既有 test_aes_smp.cpp 的 smp_c1_definition/smp_s1_definition 是"手工两步复算"，
// 但复算排布抄自实现本身（同构自证），无法裁决 global-review §3.5/#32 与 §3.7/#49
// 两组静态复核的矛盾。这里改用 Bluetooth Core 5.4 [Vol 3] Part H §2.2.3/§2.2.4
// 的官方工作样例做外部锚点（十六进制逐字节照抄规范文本，不依赖实现侧任何排布
// 选择）：
//   §2.2.3 c1：iat'=0x01, rat'=0x00, preq=0x07071000000101, pres=0x05000800000302
//     → p1 = 0x05000800000302070710000001010001（"iat' 成为 p1 最低有效字节，
//        pres 最高有效字节成为 p1 最高有效字节"，即 MSB 端起 pres||preq||rat'||iat'）
//     ia=0xA1A2A3A4A5A6, ra=0xB1B2B3B4B5B6
//     → p2 = 0x00000000A1A2A3A4A5A6B1B2B3B4B5B6（"ra 最低有效字节成为 p2 最低
//        有效字节，padding 最高有效字节成为 p2 最高有效字节"，即 MSB 端起
//        4 字节 0 || ia || ra）
//     k=0x00..00, r=0x5783D52156AD6F0E6388274EC6702EE0
//     → c1 = 0x1E1E3FEF878988EAD2A74DC5BEF13B86
//   §2.2.4 s1：k=0, r1=0x000F0E0D0C0B0A091122334455667788,
//     r2=0x010203040506070899AABBCCDDEEFF00
//     → r' = r1低64位 || r2低64位 = 0x112233445566778899AABBCCDDEEFF00（拼接，非 XOR）
//     → s1 = 0x9a1fe1f0e8b0f49b5b4216ae796da062
// 规范向量本身先用独立实现复核（2026-11-01 本机：.NET AES-128 ECB 单块 oracle，
// FIPS-197 C.1/B 双向量校准后，按上述排布计算的输出与规范文本逐字节一致）；
// Zephyr v2.0.0 subsys/bluetooth/host/smp.c 的 smp_c1/smp_s1（小端数组 + 每次加密
// 前后 bt_encrypt_le 反序）换算到 MSB-first AES 输入后与该排布完全一致。
//
// 【本对测试在当前树上按预期 FAIL —— 这就是裁决结果本身】
// 同日以 .NET oracle 复算当前 src/ble/smp.cpp 的排布（见各用例内注释的具体值）：
//   c1：p1 在 MSB 端排 preq||pres||iat||rat（preq/pres 顺序与 iat/rat 位置均与规范
//       互换），p2 排 ia_rev||ra_rev||0^4（padding 挪到另一端）
//       → 60bdb44af2a124299ccd83753fa1f3e5 ≠ 规范值；且 p1 布局差异已必然改变第一
//       次 AES 的输出，故无论 ia/ra 以哪种字节序约定传参都得不到规范值
//   s1：低 64 位做 XOR 而非拼接 → 0f465b9a15879baa2c603ace38f63e69 ≠ 规范值
// 结论：当前 Just Works confirm/STK 与任何规范一致主机（含 Windows）互操作必然
// 失配。矩阵注记"两组复核矛盾、唯一手段真机配对成功"就此闭环：规范工作样例即可
// 静态裁决，无需真机。按规范排布修复 src/ble/smp.cpp 后本对测试转绿；
// test_aes_smp.cpp 的两个同构自证用例（smp_c1_definition/smp_s1_definition）需
// 随修复同步更新（它们当前锚定的是错误排布）。
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ble/aes.h"
#include "ble/smp.h"

namespace {
constexpr uint8_t kR3SpecPreq[7] = {0x07, 0x07, 0x10, 0x00, 0x00, 0x01, 0x01};
constexpr uint8_t kR3SpecPres[7] = {0x05, 0x00, 0x08, 0x00, 0x00, 0x03, 0x02};
constexpr uint8_t kR3SpecIa[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};  // 48bit 整数 MSB-left
constexpr uint8_t kR3SpecRa[6] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6};
void r3_print_hex(const char* tag, const uint8_t* p, size_t n, const char* expect_hex) {
  std::printf("    %s = ", tag);
  for (size_t i = 0; i < n; ++i) std::printf("%02x", p[i]);
  std::printf(" (spec expects %s)\n", expect_hex);
  std::fflush(stdout);
}
}  // namespace

PB_TEST(cov_infra_smp_c1_core54_223_spec_example) {
  uint8_t k[16] = {0};
  uint8_t r[16] = {0x57, 0x83, 0xD5, 0x21, 0x56, 0xAD, 0x6F, 0x0E,
                   0x63, 0x88, 0x27, 0x4E, 0xC6, 0x70, 0x2E, 0xE0};
  static const uint8_t expect[16] = {0x1E, 0x1E, 0x3F, 0xEF, 0x87, 0x89, 0x88, 0xEA,
                                     0xD2, 0xA7, 0x4D, 0xC5, 0xBE, 0xF1, 0x3B, 0x86};
  // ia/ra 两种传参字节序约定都试：任一约定下都必须能给出规范工作样例的 confirm。
  // （当前实现按显示序传参得 60bdb44af2a124299ccd83753fa1f3e5，反序亦不同——
  //   p1 排布差异已注定两者都 != 规范值。）
  auto outDisplay = ble::SmpResponder::c1(k, r, kR3SpecPreq, kR3SpecPres, 1, kR3SpecIa, 0, kR3SpecRa);
  uint8_t iaRev[6], raRev[6];
  for (int i = 0; i < 6; ++i) {
    iaRev[i] = kR3SpecIa[5 - i];
    raRev[i] = kR3SpecRa[5 - i];
  }
  auto outRev = ble::SmpResponder::c1(k, r, kR3SpecPreq, kR3SpecPres, 1, iaRev, 0, raRev);
  bool matchDisplay = std::equal(outDisplay.begin(), outDisplay.end(), expect);
  bool matchRev = std::equal(outRev.begin(), outRev.end(), expect);
  if (!matchDisplay && !matchRev) {
    r3_print_hex("c1(ia display-order)", outDisplay.data(), 16, "1e1e3fef878988ead2a74dc5bef13b86");
    r3_print_hex("c1(ia reversed)      ", outRev.data(), 16, "1e1e3fef878988ead2a74dc5bef13b86");
  }
  PB_CHECK(matchDisplay || matchRev);
}

PB_TEST(cov_infra_smp_s1_core54_224_spec_example) {
  uint8_t k[16] = {0};
  uint8_t r1[16] = {0x00, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
                    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  uint8_t r2[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
  static const uint8_t expect[16] = {0x9A, 0x1F, 0xE1, 0xF0, 0xE8, 0xB0, 0xF4, 0x9B,
                                     0x5B, 0x42, 0x16, 0xAE, 0x79, 0x6D, 0xA0, 0x62};
  auto out = ble::SmpResponder::s1(k, r1, r2);
  if (!std::equal(out.begin(), out.end(), expect)) {
    r3_print_hex("s1                   ", out.data(), 16, "9a1fe1f0e8b0f49b5b4216ae796da062");
    std::printf("    (spec: r' = r1_low64 || r2_low64 concatenated, not XOR)\n");
    std::fflush(stdout);
  }
  PB_CHECK(std::equal(out.begin(), out.end(), expect));
}
