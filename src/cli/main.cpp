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

#include "control/commands.h"
#include "core/bytes.h"
#include "core/net.h"
#include "core/log.h"
#include "sim/simulator.h"
#include "usbip/protocol.h"

namespace {

volatile bool g_run = true;

BOOL WINAPI ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
    g_run = false;
    return TRUE;
  }
  return FALSE;
}

int run_serve(const std::vector<std::string>& loads, bool load_bt, bool attach,
              const std::string& probe, int exit_after_ms, uint16_t port) {
  pb::sim::Simulator sim;
  std::string err;
  if (!sim.start(&err, port)) {
    std::fprintf(stderr, "serve: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "phantombt: usbip server on port %u\n", (unsigned)sim.port());

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

  int probe_rc = 0;
  if (attach && !busids.empty()) {
    if (pb::sim::Simulator::find_usbip_exe().empty()) {
      std::fprintf(stderr,
                   "attach: usbip.exe not found (install usbip-win2); continuing without attach\n");
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
        probe_rc = system(probe.c_str());
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
  w1.u32be(OP_REQ_DEVLIST);
  c.send_all(w1.buf().data(), w1.buf().size(), &err);
  uint8_t h[12];
  if (!c.recv_exact(h, 12, &err) || h[11] != 1) {
    std::fprintf(stderr, "selftest: devlist failed\n");
    return 1;
  }
  std::vector<uint8_t> devblk(312 + 4);
  c.recv_exact(devblk.data(), devblk.size(), &err);

  pb::ByteWriter w2;
  w2.u32be(OP_REQ_IMPORT);
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
  std::vector<uint8_t> ret(40 + 18);
  if (!c.recv_exact(ret.data(), ret.size(), &err) || ret[40] != 18) {
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
  std::vector<uint8_t> ret2(40 + 3);
  if (!c.recv_exact(ret2.data(), ret2.size(), &err) || ret2[40] != 0xC0) {
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
    std::fprintf(stderr,
                 "usage: phantombt serve [--port N] [--load <profile.json>]... [--load-bt] "
                 "[--attach] [--probe \"<cmdline>\"] [--exit-after-ms N]\n"
                 "       phantombt selftest\n");
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "selftest") return run_selftest();

  if (cmd == "serve") {
    uint16_t port = 3240;
    std::vector<std::string> loads;
    bool load_bt = false, attach = false;
    std::string probe;
    int exit_after = 0;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
      if (a == "--port") port = (uint16_t)std::stoi(next());
      else if (a == "--load") loads.push_back(next());
      else if (a == "--load-bt") load_bt = true;
      else if (a == "--attach") attach = true;
      else if (a == "--probe") probe = next();
      else if (a == "--exit-after-ms") exit_after = std::stoi(next());
      else {
        std::fprintf(stderr, "serve: unknown flag %s\n", a.c_str());
        return 2;
      }
    }
    return run_serve(loads, load_bt, attach, probe, exit_after, port);
  }

  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
