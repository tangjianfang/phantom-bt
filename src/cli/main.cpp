// phantombt.exe：CLI 前端
// 用法：
//   phantombt serve [--port N] [--load <profile.json>]... [--attach] [--probe "<cmdline>"]
//                   [--exit-after-ms N]
//   phantombt selftest
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "cli/serve_args.h"
#include "control/commands.h"
#include "control/pipe_server.h"
#include "core/bytes.h"
#include "core/env_check.h"
#include "core/net.h"
#include "core/log.h"
#include "sim/simulator.h"
#include "usbip/protocol.h"

namespace {

volatile bool g_run = true;

void print_usage() {
  std::fprintf(stderr,
               "usage: phantombt serve [--port N] [--load <profile.json>]... [--load-bt] "
               "[--attach] [--probe \"<cmdline>\"] [--exit-after-ms N]\n"
               "       phantombt selftest\n");
}

BOOL WINAPI ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
    g_run = false;
    return TRUE;
  }
  return FALSE;
}

int run_serve(const std::vector<std::string>& loads, bool load_bt,
              const std::vector<std::string>& bles, bool attach, const std::string& probe,
              int exit_after_ms, uint16_t port) {
  pb::sim::Simulator sim;
  std::string err;
  if (!sim.start(&err, port)) {
    std::fprintf(stderr, "serve: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "phantombt: usbip server on port %u\n", (unsigned)sim.port());
  // Same control pipe as the GUI, so "phantombt cmd ..." can drive serve too.
  pb::control::PipeServer pipe(sim);
  pipe.start(&err);

  std::vector<std::string> busids;
  for (auto& p : loads) {
    try {
      busids.push_back(sim.cmd_load_hid_profile(p));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "load %s: %s\n", p.c_str(), e.what());
      sim.stop();
      return 1;
    }
  }
  if (load_bt) {
    try {
      busids.push_back(sim.cmd_load_bt_adapter());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "load-bt: %s\n", e.what());
      sim.stop();
      return 1;
    }
  }
  for (auto& b : bles) {
    try {
      sim.cmd_ble_advertise(b);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "ble %s: %s\n", b.c_str(), e.what());
      sim.stop();
      return 1;
    }
  }

  int probe_rc = 0;
  // probe-skip：--probe 给出但探针实际无法运行（无 --attach / 无已加载设备 /
  // 无 usbip.exe）时，必须显式输出 SKIPPED 并以非零码退出（3，与探针约定
  // 0=pass 2=fail 3=skip 一致，probes/main.cpp）——静默返回 0 是"未验证=通过"
  // 的假绿
  const bool can_attach = attach && !busids.empty();
  if (!can_attach && !probe.empty()) {
    std::fprintf(stderr, "probe: SKIPPED (probe requires --attach with a loaded device)\n");
    probe_rc = 3;
  }
  if (can_attach) {
    if (pb::sim::Simulator::find_usbip_exe().empty()) {
      std::fprintf(stderr,
                   "attach: usbip.exe not found (install usbip-win2); continuing without attach\n");
      if (!probe.empty()) {
        std::fprintf(stderr, "probe: SKIPPED (usbip.exe not found; probe requires attach)\n");
        probe_rc = 3;
      }
    } else {
      for (auto& b : busids) {
        try {
          sim.cmd_attach(b);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "attach %s: %s\n", b.c_str(), e.what());
        }
      }
      if (!probe.empty()) {
        std::fprintf(stderr, "probe: %s\n", probe.c_str());
        // 探针阻塞运行期间周期注入测试报告（vendor 集合 Report ID 2 + 64B 载荷）：
        // 设备枚举完成时刻不定，重复注入保证订阅建立后必有报告可读
        std::thread injector([&sim, &busids] {
          for (int i = 0; i < 10; ++i) {
            Sleep(2000);
            try {
              std::vector<uint8_t> r{0x02};
              r.insert(r.end(), 64, 0xCD);
              sim.cmd_inject(busids[0], r);
            } catch (const std::exception&) {
            }
          }
        });
        probe_rc = system(probe.c_str());
        injector.join();
      }
    }
  }

  if (exit_after_ms > 0) {
    Sleep(exit_after_ms);
  } else {
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    while (g_run) Sleep(200);
  }
  sim.stop();
  pipe.stop();
  return probe_rc;
}

// 进程内回环自检：devlist → import → GET_DESCRIPTOR → 中断注入
int run_selftest() {
  pb::net::WsaSession wsa;
  pb::sim::Simulator sim;
  std::string err;
  if (!sim.start(&err, 0)) {
    std::fprintf(stderr, "selftest: %s\n", err.c_str());
    return 1;
  }
  std::string busid;
  try {
    busid = sim.cmd_load_hid_profile("profiles/vendor.json");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "selftest: %s\n", e.what());
    return 1;
  }
  auto c = pb::net::TcpConn::connect("127.0.0.1", sim.port(), &err);
  if (!c.is_open()) {
    std::fprintf(stderr, "selftest: connect: %s\n", err.c_str());
    return 1;
  }
  using namespace pb::usbip;
  pb::ByteWriter w1;
  // op_common：version(0x0111) + code + status（与真实 usbip 客户端一致）
  w1.u16be(OP_VERSION);
  w1.u16be(OP_REQ_DEVLIST);
  w1.u32be(0);
  c.send_all(w1.buf().data(), w1.buf().size(), &err);
  uint8_t h[12];
  if (!c.recv_exact(h, 12, &err) || h[11] != 1) {
    std::fprintf(stderr, "selftest: devlist failed\n");
    return 1;
  }
  std::vector<uint8_t> devblk(312 + 4);
  c.recv_exact(devblk.data(), devblk.size(), &err);

  pb::ByteWriter w2;
  w2.u16be(OP_VERSION);
  w2.u16be(OP_REQ_IMPORT);
  w2.u32be(0);
  char bid[32] = {};
  std::strncpy(bid, busid.c_str(), 31);
  w2.bytes((const uint8_t*)bid, 32);
  c.send_all(w2.buf().data(), w2.buf().size(), &err);
  uint8_t imp[8];
  if (!c.recv_exact(imp, 8, &err) || imp[7] != 0) {
    std::fprintf(stderr, "selftest: import failed\n");
    return 1;
  }
  std::vector<uint8_t> idev(312);
  c.recv_exact(idev.data(), idev.size(), &err);

  // GET_DESCRIPTOR device
  pb::ByteWriter w3;
  w3.u32be(USBIP_CMD_SUBMIT);
  w3.u32be(1);
  w3.u32be(0x00070001);
  w3.u32be(DIR_IN);
  w3.u32be(0);
  w3.u32be(0);
  w3.u32be(18);
  w3.u32be(0);
  w3.u32be(0);
  w3.u32be(0);
  uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  w3.bytes(setup, 8);
  c.send_all(w3.buf().data(), w3.buf().size(), &err);
  std::vector<uint8_t> ret(RET_SUBMIT_HDR_SIZE + 18);
  if (!c.recv_exact(ret.data(), ret.size(), &err) || ret[RET_SUBMIT_HDR_SIZE] != 18) {
    std::fprintf(stderr, "selftest: descriptor failed\n");
    return 1;
  }

  // 中断 IN 挂起 + 注入
  pb::ByteWriter w4;
  w4.u32be(USBIP_CMD_SUBMIT);
  w4.u32be(2);
  w4.u32be(0x00070001);
  w4.u32be(DIR_IN);
  w4.u32be(0x81);
  w4.u32be(0);
  w4.u32be(64);
  w4.u32be(0);
  w4.u32be(0);
  w4.u32be(0);
  w4.u32be(10);
  uint8_t zero8[8] = {};
  w4.bytes(zero8, 8);
  c.send_all(w4.buf().data(), w4.buf().size(), &err);
  Sleep(150);
  sim.cmd_inject(busid, {0xC0, 0xFF, 0xEE});
  std::vector<uint8_t> ret2(RET_SUBMIT_HDR_SIZE + 3);
  if (!c.recv_exact(ret2.data(), ret2.size(), &err) || ret2[RET_SUBMIT_HDR_SIZE] != 0xC0) {
    std::fprintf(stderr, "selftest: interrupt inject failed\n");
    return 1;
  }

  c.close();
  sim.stop();
  std::printf("PASS loopback (devlist/import/descriptor/interrupt)\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "selftest") return run_selftest();

  if (cmd == "cmd" && argc >= 3) {
    // Send one control command to a running test bench (GUI or serve).
    std::vector<std::string> replies;
    if (!pb::control::CommandClient::send_line("\\\\.\\pipe\\phantom-bt", argv[2], replies, 3000)) {
      std::fprintf(stderr, "no test bench is running (pipe \\\\.\\pipe\\phantom-bt)\n");
      return 1;
    }
    for (auto& r : replies) std::printf("%s\n", r.c_str());
    return 0;
  }

  if (cmd == "doctor") {
    auto checks = pb::env::run_checks();
    int bad = 0;
    for (auto& c : checks) {
      const char* mark = c.status == pb::env::Status::Ok
                             ? "[ OK ]"
                             : (c.status == pb::env::Status::Warn
                                    ? "[WARN]"
                                    : (c.status == pb::env::Status::Info ? "[INFO]" : "[FAIL]"));
      // Info rows are informational: same predicate the GUI first-run check uses.
      if (c.status == pb::env::Status::Warn || c.status == pb::env::Status::Fail) ++bad;
      std::printf("%s %s\n      %s\n", mark, c.title.c_str(), c.detail.c_str());
      if (!c.action_url.empty())
        std::printf("      -> %s (%s)\n", c.action_label.c_str(), c.action_url.c_str());
    }
    std::printf("== %zu 项检测，%d 项需处理 ==\n", checks.size(), bad);
    return bad ? 1 : 0;
  }

  if (cmd == "serve") {
    // bug cli-stoi：--port/--exit-after-ms 的缺值/非法值在这里变成"用法错误 +
    // 退出码 2"，而不是未捕获的 std::stoi 异常（那会触发 Windows 错误报告
    // 模态框）。安全解析封装见 cli/serve_args.h。
    pb::cli::ServeArgs a =
        pb::cli::parse_serve_args(std::vector<std::string>(argv + 2, argv + argc));
    if (!a.ok) {
      std::fprintf(stderr, "serve: %s\n", a.error.c_str());
      print_usage();
      return 2;
    }
    return run_serve(a.loads, a.load_bt, a.bles, a.attach, a.probe, a.exit_after_ms, a.port);
  }

  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
