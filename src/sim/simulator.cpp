#include "sim/simulator.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <utility>

#include <windows.h>

#include "core/bt_radio.h"
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
  // 以下四个容器/指针被命令线程（GUI/CLI/pipe）与 tick 线程并发访问，
  // 统一由 mx 保护（DeviceRegistry 自带锁，不在此列）
  std::map<std::string, std::shared_ptr<hid::HidModel>> hid_models;  // busid → hid 模型
  std::map<std::string, std::shared_ptr<bt::AdapterModel>> bt_models;
  // BLE 虚拟设备（挂在首个 bt 适配器的空口上）+ 设备栈（ATT/GATT/HOGP/SMP）
  std::map<uint64_t, ble::BleDeviceDef> ble_devices;
  std::unique_ptr<ble::DeviceStack> stack;
  std::thread tick_thread;
  std::atomic<bool> ticking{false};
  mutable std::mutex mx;
  // 锁内拷贝首个适配器（shared_ptr 延长生命周期，unload erase 后 tick 仍安全）
  std::shared_ptr<bt::AdapterModel> adapter_locked() const {
    std::lock_guard<std::mutex> lk(mx);
    return bt_models.empty() ? nullptr : bt_models.begin()->second;
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
          // 拷贝 shared_ptr 后在锁外 tick：unload 可并发 erase，旧对象存活至本帧结束
          if (auto ad = impl_->adapter_locked()) ad->controller().tick(t);
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

namespace {

// ASCII → HOGP boot 键盘报告（修饰键字节, HID Usage ID）。
// HID Usage Tables：'a'-'z'=0x04-0x1D、'1'-'9'=0x1E-0x26、'0'=0x27、空格 0x2C、
// 回车 0x28、退格 0x2A；大写字母加 LShift(0x02)。不可映射返回 {0,0}。
std::pair<uint8_t, uint8_t> ascii_to_hid_key(char c) {
  uint8_t mod = 0;
  char base = c;
  if (c >= 'A' && c <= 'Z') {
    mod = 0x02;  // Left Shift
    base = (char)(c - 'A' + 'a');
  }
  const uint8_t kNone = 0;
  if (base >= 'a' && base <= 'z') return {mod, (uint8_t)(0x04 + (base - 'a'))};
  if (c >= '1' && c <= '9') return {kNone, (uint8_t)(0x1E + (c - '1'))};
  if (c == '0') return {kNone, uint8_t{0x27}};
  if (c == ' ') return {kNone, uint8_t{0x2C}};
  if (c == '\n' || c == '\r') return {kNone, uint8_t{0x28}};
  if (c == '\b' || c == '\t') return {kNone, (uint8_t)(c == '\b' ? 0x2A : 0x2B)};
  return {kNone, kNone};
}

}  // namespace

namespace {
// 把适配器控制器的四个收发点挂到抓包写者（on=false 摘钩）。
// 捕获写者指针而非 Simulator*：与 snoop_ 成员同生命周期，语义等价且更窄。
void wire_snoop(bt::AdapterModel* m, sim::BtSnoopWriter* w, bool on) {
  auto& ctrl = m->controller();
  if (on) {
    ctrl.snoop_command = [w](const std::vector<uint8_t>& c) { w->hci_command(c); };
    ctrl.snoop_event = [w](const std::vector<uint8_t>& e) { w->hci_event(e); };
    ctrl.snoop_acl_in = [w](const hci::AclFrame& f) { w->acl_from_host(f); };
    ctrl.snoop_acl_out = [w](const hci::AclFrame& f) { w->acl_to_host(f); };
  } else {
    ctrl.snoop_command = nullptr;
    ctrl.snoop_event = nullptr;
    ctrl.snoop_acl_in = nullptr;
    ctrl.snoop_acl_out = nullptr;
  }
}
}  // namespace

std::string Simulator::cmd_snoop(const std::string& path_or_off) {
  if (path_or_off == "off") {
    snoop_off_ = true;
    snoop_.close();
    std::lock_guard<std::mutex> lk(impl_->mx);
    for (auto& [busid, m] : impl_->bt_models) wire_snoop(m.get(), &snoop_, false);
    PB_LOG_INFO("sim: snoop off");
    return "snoop off";
  }
  snoop_.close();  // BtSnoopWriter::open 对已打开的写者恒成功，须先关再开
  if (!snoop_.open(path_or_off)) throw std::runtime_error("cannot open " + path_or_off);
  snoop_off_ = false;
  std::lock_guard<std::mutex> lk(impl_->mx);
  for (auto& [busid, m] : impl_->bt_models) wire_snoop(m.get(), &snoop_, true);
  PB_LOG_INFO("sim: snoop -> " + path_or_off);
  return "snoop " + path_or_off;
}

std::string Simulator::cmd_ble_advertise(const std::string& json_path) {
  auto def = ble::BleDeviceDef::load_file(json_path);
  auto adv = std::make_shared<hci::VirtualAdvertiser>(def.address, def.name, def.connectable);
  adv->set_adv_data_name(def.name);
  adv->set_scan_response(def.name);
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    if (impl_->bt_models.empty()) throw std::runtime_error("no bt adapter loaded (run load-bt first)");
    impl_->bt_models.begin()->second->controller().air().add(std::move(adv));
    impl_->ble_devices[def.address] = def;
    if (impl_->stack && def.kind == "keyboard") impl_->stack->setup_keyboard(def.name);
  }
  PB_LOG_INFO("sim: ble advertising '" + def.name + "' @ " +
              ble::BleDeviceDef::format_address(def.address));
  return "OK " + ble::BleDeviceDef::format_address(def.address);
}

std::string Simulator::cmd_ble_stop(const std::string& addr_hex) {
  uint64_t addr = ble::BleDeviceDef::parse_address(addr_hex);
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    if (impl_->bt_models.empty()) throw std::runtime_error("no bt adapter loaded");
    impl_->bt_models.begin()->second->controller().air().remove(addr);
    impl_->ble_devices.erase(addr);
  }
  return "stopped";
}

std::string Simulator::cmd_ble_clear() {
  std::lock_guard<std::mutex> lk(impl_->mx);
  size_t n = impl_->ble_devices.size();
  if (!impl_->bt_models.empty()) {
    auto* ad = impl_->bt_models.begin()->second.get();
    for (auto& [addr, def] : impl_->ble_devices) ad->controller().air().remove(addr);
  }
  impl_->ble_devices.clear();
  return "cleared " + std::to_string(n) + " ble device(s)";
}

std::string Simulator::cmd_ble_list() {
  std::lock_guard<std::mutex> lk(impl_->mx);
  std::string out;
  for (auto& [addr, def] : impl_->ble_devices) {
    if (!out.empty()) out += "\n";
    out += ble::BleDeviceDef::format_address(addr) + " " + def.kind + " " + def.name;
  }
  return out;
}

std::string Simulator::cmd_ble_type(const std::string& addr_hex, const std::string& text) {
  std::lock_guard<std::mutex> lk(impl_->mx);  // stack 可能被并发 unload reset，全程持锁
  if (!impl_->stack) throw std::runtime_error("no bt adapter loaded");
  if (impl_->bt_models.empty() || !impl_->bt_models.begin()->second->controller().has_connection())
    throw std::runtime_error("no ble connection (connect from host first)");
  // 签名承诺按设备寻址：addr 非空时校验确为已注册的广播设备
  if (!addr_hex.empty()) {
    uint64_t addr = ble::BleDeviceDef::parse_address(addr_hex);
    if (impl_->ble_devices.count(addr) == 0)
      throw std::runtime_error("no ble device " + addr_hex + " (advertise it first)");
  }
  int sent = 0;
  for (char c : text) {
    auto [mod, key] = ascii_to_hid_key(c);
    if (key == 0) continue;  // 不可映射字符忽略（避免错误按键）
    impl_->stack->send_input_report({mod, 0x00, key, 0, 0, 0, 0, 0});
    ++sent;
    impl_->stack->send_input_report({0, 0, 0, 0, 0, 0, 0, 0});  // 抬起
  }
  return "typed " + std::to_string(sent) + " chars";
}

std::string Simulator::cmd_load_hid_profile(const std::string& json_path) {
  auto prof = hid::HidProfile::load_file(json_path);
  uint32_t devnum;
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    devnum = impl_->next_devnum++;
  }
  auto model = std::make_shared<hid::HidModel>(prof, 7, devnum);
  std::string busid = impl_->reg.add(model);
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->hid_models[busid] = model;
  }
  char vidpid[16];
  std::snprintf(vidpid, sizeof(vidpid), "%04x:%04x", prof.vid, prof.pid);
  PB_LOG_INFO("sim: loaded hid profile '" + json_path + "' as " + busid + " (" + vidpid + ")");
  return busid;
}

std::string Simulator::cmd_load_bt_adapter() {
  uint32_t devnum;
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    devnum = impl_->next_devnum++;
  }
  auto model = std::make_shared<bt::AdapterModel>(pb::hci::ControllerConfig{}, 7, devnum);
  std::string busid = impl_->reg.add(model);
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->bt_models[busid] = model;
    // 装配 BLE 设备栈（ATT/GATT/HOGP/SMP 绑定到该控制器）
    impl_->stack = std::make_unique<ble::DeviceStack>(model->controller());
  }
  // HCI 抓包（默认 integration-results/hci.snoop，Wireshark 可开；失败不阻断）。
  // 已被 snoop <path> 重定向 → 沿用该文件；snoop off 后不再自动开（保持关闭）。
  if (!snoop_off_ && !snoop_.is_open()) (void)snoop_.open("integration-results/hci.snoop");
  if (snoop_.is_open()) wire_snoop(model.get(), &snoop_, true);
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

std::string Simulator::cmd_unload(const std::string& busid) {
  if (!impl_->reg.find(busid)) throw std::runtime_error("unknown busid " + busid);
  // Best-effort detach first, so no usbip client still references the model.
  try {
    (void)cmd_detach(busid);
  } catch (...) {
  }
  impl_->reg.remove(busid);
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    bool was_bt = impl_->bt_models.count(busid) > 0;
    if (was_bt) {
      // 先销毁设备栈：它引用本适配器的 controller，erase 使 shared_ptr 归零时
      // AdapterModel 立即析构 —— 后 reset 会让 ~DeviceStack 触碰悬垂 controller（UAF）
      impl_->stack.reset();
      // BLE devices hang off this adapter's air — they must go with it.
      if (auto it = impl_->bt_models.find(busid); it != impl_->bt_models.end()) {
        auto* dying = it->second.get();
        for (auto& [addr, def] : impl_->ble_devices) dying->controller().air().remove(addr);
      }
      impl_->ble_devices.clear();
    }
    impl_->hid_models.erase(busid);
    impl_->bt_models.erase(busid);
    if (was_bt && !impl_->bt_models.empty())  // another adapter remains: rewire
      impl_->stack =
          std::make_unique<ble::DeviceStack>(impl_->bt_models.begin()->second->controller());
  }
  PB_LOG_INFO("sim: unloaded " + busid);
  return "unloaded " + busid;
}

std::string Simulator::cmd_unload_all() {
  auto devs = impl_->reg.list();
  int n = 0;
  for (auto& d : devs) {
    try {
      (void)cmd_unload(d.busid);
      ++n;
    } catch (...) {
    }
  }
  return "unloaded " + std::to_string(n) + " device(s)";
}

std::string Simulator::cmd_sys_bt(const std::string& op) {
  if (op == "list") {
    auto radios = btradio::list_radios();
    if (radios.empty()) return "no physical bluetooth radio found";
    std::string out;
    for (auto& r : radios) {
      if (!out.empty()) out += "\n";
      out += std::string(r.disabled ? "[disabled] " : "[enabled]  ") +
             (r.physical ? "RADIO " : "child  ") + r.description + " (" + r.instance_id + ")";
    }
    return out;
  }
  if (op != "disable" && op != "enable")
    throw std::runtime_error("usage: sys-bt <list|disable|enable>");
  std::string err;
  int n = btradio::set_radios_enabled(op == "enable", &err);
  if (n == 0)
    throw std::runtime_error(err.empty() ? "no radio changed (admin required?)" : err);
  return (op == "enable" ? "enabled " : "disabled ") + std::to_string(n) + " radio(s)" +
         (err.empty() ? "" : " (" + err + ")");
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

// busid 白名单 ^\d+-\d+$：busid 会原样内插进 _popen 命令串（cmd.exe 解释），
// 元字符/引号即命令注入；仅放行 "<busnum>-<devnum>"（恰好一个 '-'、两侧各
// 至少一位数字、其余全为数字）
static bool valid_busid(const std::string& s) {
  size_t dash = s.find('-');
  if (dash == std::string::npos || dash == 0 || dash + 1 == s.size()) return false;
  for (size_t i = 0; i < s.size(); ++i)
    if (i != dash && (s[i] < '0' || s[i] > '9')) return false;  // 含第二个 '-' 亦拒
  return true;
}

// 内嵌服务器端口约定（bug gui-port）：PB_PORT（十进制 1-65535），未设置/非法
// 回落默认 3425 —— usbip 客户端默认端口 3240 被本机其它项目常驻占用。
uint16_t Simulator::server_port_from_env() {
  constexpr uint16_t kDefaultPort = 3425;
  char buf[16];
  DWORD n = GetEnvironmentVariableA("PB_PORT", buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return kDefaultPort;  // 未设置或异常超长
  unsigned long v = 0;
  for (DWORD i = 0; i < n; ++i) {
    if (buf[i] < '0' || buf[i] > '9') return kDefaultPort;
    v = v * 10 + (unsigned long)(buf[i] - '0');
    if (v > 65535) return kDefaultPort;
  }
  return v == 0 ? kDefaultPort : (uint16_t)v;
}

std::vector<std::string> Simulator::attach_commands(const std::string& exe, uint16_t port,
                                                    const std::string& busid) {
  // 新旧两种语法依次尝试：usbip-win2 的 -t 是全局选项（须在子命令之前），仅在非
  // usbip 默认端口（3240）时携带；旧版 usbip-win 用 --remote/--port/--busid。
  // GUI 默认跑在 3425（server_port_from_env）→ 从 GUI 发起的 attach 恒带 -t。
  // 新语法分支追加 --receive-mode low-latency：usbip-win2 0.9.8 默认 zero-copy 接收
  // 模式存在"在解锁内存页前完成传输"缺陷，HCI 事件（中断 IN）恰在该路径上，间歇丢事件。
  std::string port_opt = port == 3240 ? "" : " -t " + std::to_string(port);
  return {
      "\"" + exe + "\"" + port_opt + " attach -r 127.0.0.1 -b " + busid +
          " --receive-mode low-latency",
      "\"" + exe + "\" attach --remote=127.0.0.1 --port=" + std::to_string(port) +
          " --busid=" + busid,
  };
}

// "usbip port" 输出解析（bug detach-by-device）。实测格式（usbip-win2 0.9.8）：
//   Port 01: device in use at High Speed(480Mbps)
//              -> usbip://127.0.0.1:3240/1-1
// 每个 "Port NN:" 块内首个 "-> usbip://host:port/<busid>" 行给出该 hub 口导入的
// busid（可能是任何服务器的设备——映射只认导入块本身，卸载决策按 busid 精确匹配）。
std::map<std::string, int> Simulator::parse_imported_ports(const std::string& port_output) {
  std::map<std::string, int> out;
  std::istringstream ss(port_output);
  std::string line;
  int cur = -1;  // 最近一个 "Port NN:" 的端口号；无有效块头时 -1
  while (std::getline(ss, line)) {
    if (line.rfind("Port ", 0) == 0 || line.rfind("port ", 0) == 0) {
      cur = -1;
      size_t colon = line.find(':');
      if (colon == std::string::npos || colon <= 5) continue;
      unsigned long v = 0;
      bool good = true;
      for (size_t i = 5; i < colon; ++i) {
        if (line[i] < '0' || line[i] > '9') {
          good = false;
          break;
        }
        v = v * 10 + (unsigned long)(line[i] - '0');
        if (v > 255) {  // usbip detach -p 合法范围 1-255
          good = false;
          break;
        }
      }
      if (good) cur = (int)v;
    } else if (cur >= 0) {
      size_t u = line.find("-> usbip://");
      if (u == std::string::npos) continue;
      size_t slash = line.find('/', u + 11);  // busid 前的斜杠（跳过 "://"）
      if (slash == std::string::npos) continue;
      std::string busid = line.substr(slash + 1);
      while (!busid.empty() &&
             (busid.back() == '\r' || busid.back() == ' ' || busid.back() == '\t'))
        busid.pop_back();
      if (!busid.empty()) out[busid] = cur;
    }
  }
  return out;
}

std::string Simulator::detach_command_for(const std::string& exe, const std::string& port_output,
                                          const std::string& busid) {
  auto imported = parse_imported_ports(port_output);
  auto it = imported.find(busid);
  if (it == imported.end()) return "";  // 未导入：无事可做（绝不 --all 波及别的设备）
  return "\"" + exe + "\" detach -p " + std::to_string(it->second);
}

std::string Simulator::cmd_attach(const std::string& busid) {
  if (!valid_busid(busid))
    throw std::runtime_error("invalid busid '" + busid + "' (expect <busnum>-<devnum>)");
  std::string exe = find_usbip_exe();
  if (exe.empty()) {
    throw std::runtime_error(
        "usbip.exe not found; install usbip-win2 (https://github.com/vadimgrn/usbip-win2)");
  }
  for (auto& cmd : attach_commands(exe, port_, busid)) {
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
  // 按设备卸载（bug detach-by-device）：解析 "usbip port" 输出建立 busid→端口
  // 映射后 detach -p <n>；未导入则无事可做 —— 不再恒 detach --all（那会波及
  // 本机其它项目挂载的设备，如常驻 3240 端口服务器的导入）。
  std::string out;
  {
    std::string list_cmd = "\"" + exe + "\" port";
    PB_LOG_INFO("sim: " + list_cmd);
    FILE* f = _popen(list_cmd.c_str(), "r");
    if (f) {
      char buf[512];
      while (fgets(buf, sizeof(buf), f)) out += buf;
      _pclose(f);
    }
  }
  std::string cmd = detach_command_for(exe, out, busid);
  if (cmd.empty()) return "not attached";
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
  std::shared_ptr<hid::HidModel> model;
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    auto it = impl_->hid_models.find(busid);
    if (it == impl_->hid_models.end()) throw std::runtime_error("no hid device " + busid);
    model = it->second;
  }
  model->inject_input_report(report);  // HidModel 自带锁
  return "injected " + std::to_string(report.size()) + " bytes";
}

std::string Simulator::cmd_status() {
  return "usbip-port=" + std::to_string(port_) + " devices=" + std::to_string(impl_->reg.size());
}

}  // namespace pb::sim
