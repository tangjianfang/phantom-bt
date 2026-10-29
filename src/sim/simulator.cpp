#include "sim/simulator.h"

#include <cstdio>
#include <cstdlib>

#include <windows.h>

#include "core/bytes.h"
#include "core/log.h"
#include "core/net.h"
#include "bluetooth/adapter_model.h"
#include "ble/ble_device.h"
#include "ble/device_stack.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"

namespace pb::sim {

struct Simulator::Impl {
  DeviceRegistry reg;
  std::unique_ptr<usbip::Server> server;
  uint32_t next_devnum = 1;
  std::map<std::string, std::shared_ptr<hid::HidModel>> hid_models;  // busid → hid 模型
  std::map<std::string, std::shared_ptr<bt::AdapterModel>> bt_models;
  // BLE 虚拟设备（挂在首个 bt 适配器的空口上）+ 设备栈（ATT/GATT/HOGP/SMP）
  std::map<uint64_t, ble::BleDeviceDef> ble_devices;
  std::unique_ptr<ble::DeviceStack> stack;
  std::thread tick_thread;
  std::atomic<bool> ticking{false};
  bt::AdapterModel* adapter_for_ble() {
    return bt_models.empty() ? nullptr : bt_models.begin()->second.get();
  }
};

Simulator::Simulator() : impl_(std::make_unique<Impl>()) {}
Simulator::~Simulator() { stop(); }

bool Simulator::start(std::string* err, uint16_t want_port) {
  static net::WsaSession wsa_once;  // 进程级 Winsock 初始化（引用计数安全）
  uint16_t last = want_port == 0 ? 0 : want_port + 9;
  for (uint16_t p = want_port; p <= last; ++p) {
    impl_->server = std::make_unique<usbip::Server>(impl_->reg, p);
    if (impl_->server->start(err)) {
      port_ = impl_->server->port();
      impl_->ticking = true;
      impl_->tick_thread = std::thread([this] {
        uint64_t t = 0;
        while (impl_->ticking.load()) {
          if (auto* ad = impl_->adapter_for_ble()) ad->controller().tick(t);
          Sleep(16);
          t += 16;
        }
      });
      return true;
    }
    PB_LOG_WARN("usbip: port " + std::to_string(p) + " unavailable: " + (err ? *err : "?"));
  }
  return false;
}

void Simulator::stop() {
  if (impl_->ticking.exchange(false) && impl_->tick_thread.joinable()) impl_->tick_thread.join();
  if (impl_->server) {
    impl_->server->stop();
    impl_->server.reset();
  }
}

std::string Simulator::cmd_ble_advertise(const std::string& json_path) {
  auto* ad = impl_->adapter_for_ble();
  if (!ad) throw std::runtime_error("no bt adapter loaded (run load-bt first)");
  auto def = ble::BleDeviceDef::load_file(json_path);
  auto adv = std::make_shared<hci::VirtualAdvertiser>(def.address, def.name, def.connectable);
  adv->set_adv_data_name(def.name);
  adv->set_scan_response(def.name);
  ad->controller().air().add(std::move(adv));
  impl_->ble_devices[def.address] = def;
  if (impl_->stack && def.kind == "keyboard") impl_->stack->setup_keyboard(def.name);
  PB_LOG_INFO("sim: ble advertising '" + def.name + "' @ " +
              ble::BleDeviceDef::format_address(def.address));
  return "OK " + ble::BleDeviceDef::format_address(def.address);
}

std::string Simulator::cmd_ble_stop(const std::string& addr_hex) {
  auto* ad = impl_->adapter_for_ble();
  if (!ad) throw std::runtime_error("no bt adapter loaded");
  uint64_t addr = ble::BleDeviceDef::parse_address(addr_hex);
  ad->controller().air().remove(addr);
  impl_->ble_devices.erase(addr);
  return "stopped";
}

std::string Simulator::cmd_ble_list() {
  std::string out;
  for (auto& [addr, def] : impl_->ble_devices) {
    if (!out.empty()) out += "\n";
    out += ble::BleDeviceDef::format_address(addr) + " " + def.kind + " " + def.name;
  }
  return out;
}

std::string Simulator::cmd_ble_type(const std::string& addr_hex, const std::string& text) {
  if (!impl_->stack) throw std::runtime_error("no bt adapter loaded");
  auto* ad = impl_->adapter_for_ble();
  if (!ad->controller().has_connection())
    throw std::runtime_error("no ble connection (connect from host first)");
  int sent = 0;
  for (char c : text) {
    impl_->stack->hogp().send_input_report({0x00, 0x00, (uint8_t)c, 0, 0, 0, 0, 0});
    ++sent;
    impl_->stack->hogp().send_input_report({0, 0, 0, 0, 0, 0, 0, 0});
  }
  return "typed " + std::to_string(sent) + " chars";
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

std::string Simulator::cmd_load_bt_adapter() {
  uint32_t devnum = impl_->next_devnum++;
  auto model = std::make_shared<bt::AdapterModel>(pb::hci::ControllerConfig{}, 7, devnum);
  std::string busid = impl_->reg.add(model);
  impl_->bt_models[busid] = model;
  // 装配 BLE 设备栈（ATT/GATT/HOGP/SMP 绑定到该控制器）
  impl_->stack = std::make_unique<ble::DeviceStack>(model->controller());
  // HCI 抓包（Wireshark btsnoop；失败不阻断）
  auto& ctrl = model->controller();
  if (snoop_.open("integration-results/hci.snoop")) {
    ctrl.snoop_command = [this](const std::vector<uint8_t>& c) { snoop_.hci_command(c); };
    ctrl.snoop_event = [this](const std::vector<uint8_t>& e) { snoop_.hci_event(e); };
    ctrl.snoop_acl_in = [this](const hci::AclFrame& f) { snoop_.acl_from_host(f); };
    ctrl.snoop_acl_out = [this](const hci::AclFrame& f) { snoop_.acl_to_host(f); };
  }
  PB_LOG_INFO("sim: loaded bt adapter as " + busid);
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
