#include "sim/simulator.h"

#include <cstdio>
#include <cstdlib>

#include <windows.h>

#include "core/bytes.h"
#include "core/log.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"

namespace pb::sim {

struct Simulator::Impl {
  DeviceRegistry reg;
  std::unique_ptr<usbip::Server> server;
  uint32_t next_devnum = 1;
  std::map<std::string, std::shared_ptr<hid::HidModel>> hid_models;  // busid → hid 模型
};

Simulator::Simulator() : impl_(std::make_unique<Impl>()) {}
Simulator::~Simulator() { stop(); }

bool Simulator::start(std::string* err, uint16_t want_port) {
  for (uint16_t p = want_port; p < want_port + 10; ++p) {
    impl_->server = std::make_unique<usbip::Server>(impl_->reg, p);
    if (impl_->server->start(err)) {
      port_ = impl_->server->port();
      return true;
    }
    PB_LOG_WARN("usbip: port " + std::to_string(p) + " unavailable: " + (err ? *err : "?"));
  }
  return false;
}

void Simulator::stop() {
  if (impl_->server) {
    impl_->server->stop();
    impl_->server.reset();
  }
}

std::string Simulator::cmd_load_hid_profile(const std::string& json_path) {
  auto prof = hid::HidProfile::load_file(json_path);
  uint32_t devnum = impl_->next_devnum++;
  auto model = std::make_shared<hid::HidModel>(prof, 7, devnum);
  std::string busid = impl_->reg.add(model);
  impl_->hid_models[busid] = model;
  char vidpid[16];
  std::snprintf(vidpid, sizeof(vidpid), "%04x:%04x", prof.vid, prof.pid);
  PB_LOG_INFO("sim: loaded hid profile '" + json_path + "' as " + busid + " (" + vidpid + ")");
  return busid;
}

std::string Simulator::cmd_list() {
  auto devs = impl_->reg.list();
  std::string out;
  for (auto& d : devs) {
    char line[256];
    std::snprintf(line, sizeof(line), "%s %04x:%04x %s", d.busid.c_str(), d.vid, d.pid,
                  d.path.c_str());
    if (!out.empty()) out += "\n";
    out += line;
  }
  return out;
}

std::string Simulator::find_usbip_exe() {
  const char* kCandidates[] = {
      "C:\\Program Files\\USBip\\usbip.exe",
      "C:\\Program Files (x86)\\USBip\\usbip.exe",
  };
  for (auto* p : kCandidates) {
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return p;
  }
  // PATH 上找
  char pathbuf[MAX_PATH];
  DWORD n = SearchPathA(nullptr, "usbip.exe", nullptr, MAX_PATH, pathbuf, nullptr);
  if (n > 0 && n < MAX_PATH) return std::string(pathbuf, n);
  return "";
}

std::string Simulator::cmd_attach(const std::string& busid) {
  std::string exe = find_usbip_exe();
  if (exe.empty()) {
    throw std::runtime_error(
        "usbip.exe not found; install usbip-win2 (https://github.com/vadimgrn/usbip-win2)");
  }
  // 新旧两种语法依次尝试
  std::string cmds[2] = {
      "\"" + exe + "\" attach --remote=127.0.0.1 --port=" + std::to_string(port_) +
          " --busid=" + busid,
      "\"" + exe + "\" attach -r 127.0.0.1 -p " + std::to_string(port_) + " -b " + busid,
  };
  for (auto& cmd : cmds) {
    PB_LOG_INFO("sim: " + cmd);
    FILE* f = _popen(cmd.c_str(), "r");
    if (!f) continue;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
      std::string line(buf);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (!line.empty()) PB_LOG_INFO("usbip: " + line);
    }
    int rc = _pclose(f);
    if (rc == 0) return "attached " + busid;
    PB_LOG_WARN("sim: attach attempt rc=" + std::to_string(rc));
  }
  throw std::runtime_error("usbip attach failed for " + busid);
}

std::string Simulator::cmd_detach(const std::string& busid) {
  std::string exe = find_usbip_exe();
  if (exe.empty()) throw std::runtime_error("usbip.exe not found");
  // detach 按端口：usbip-win2 的 port 映射未知时用 detach-all 语法尝试
  std::string cmd = "\"" + exe + "\" detach --all";
  PB_LOG_INFO("sim: " + cmd);
  FILE* f = _popen(cmd.c_str(), "r");
  if (f) {
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
    }
    _pclose(f);
  }
  return "detached";
}

std::string Simulator::cmd_inject(const std::string& busid, const std::vector<uint8_t>& report) {
  auto it = impl_->hid_models.find(busid);
  if (it == impl_->hid_models.end()) throw std::runtime_error("no hid device " + busid);
  it->second->inject_input_report(report);
  return "injected " + std::to_string(report.size()) + " bytes";
}

std::string Simulator::cmd_status() {
  return "usbip-port=" + std::to_string(port_) + " devices=" + std::to_string(impl_->reg.size());
}

}  // namespace pb::sim
