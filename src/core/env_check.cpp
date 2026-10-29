#include "core/env_check.h"

#include <cstdio>
#include <vector>

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include "core/bytes.h"

namespace pb::env {

// Any "USB\Class_E0&SubClass_01*" entry in a REG_MULTI_SZ CompatibleIDs blob
// marks a Bluetooth transport. SubClass must be pinned to 01: bare Class_E0
// also covers SubClass_02 (UWB / Certified Wireless USB) false positives.
bool has_class_e0_compatid(const char* multi_sz) {
  if (!multi_sz) return false;
  static constexpr char kSig[] = "USB\\Class_E0&SubClass_01";  // length derived
  for (const char* p = multi_sz; *p; p += strlen(p) + 1)
    if (_strnicmp(p, kSig, sizeof(kSig) - 1) == 0) return true;
  return false;
}

std::string usbip_download_url() { return "https://github.com/vadimgrn/usbip-win2/releases"; }
std::string usbip_docs_url() { return "https://github.com/vadimgrn/usbip-win2#readme"; }

bool os_build_ok(uint32_t build_number) { return build_number >= 18362; }

namespace {
// Known Bluetooth radio vendors — the SECOND detection axis. The class probe
// (Service/Class/CompatibleIDs) is authoritative, but vendor-specific radios
// (recovery firmware, off-spec CSR clones) expose no E0/01 descriptors; the
// VID list keeps those detectable.
bool known_bt_vid(const std::string& vid) {
  static const char* kVids[] = {
      "VID_0A12",  // Cambridge Silicon Radio (CSR)
      "VID_0A5C",  // Broadcom
      "VID_8087",  // Intel
      "VID_0489",  // Foxconn / MediaTek
      "VID_0B05",  // ASUS
      "VID_04CA",  // IMC Networks
      "VID_13D3",  // IMC Networks
      "VID_0BB4",  // HTC
      "VID_050D",  // Belkin
      "VID_1286",  // Marvell
      "VID_2357",  // TP-Link
      "VID_2604",  // Generic dongle
      "VID_1A86",  // WCH
  };
  for (auto* v : kVids)
    if (vid == v) return true;
  return false;
}
}  // namespace

bool foreign_vid_enum_key(const std::string& key, const std::string& our_vid) {
  if (key.rfind("VID_", 0) != 0) return false;
  size_t amp = key.find('&');
  if (amp == std::string::npos) return false;
  return key.substr(0, amp) != our_vid;  // our own virtual adapter excluded
}

std::string classify_os(uint32_t build, uint32_t major, uint32_t minor) {
  char buf[96];
  if (major >= 10 && build >= 22000)
    std::snprintf(buf, sizeof(buf), "Windows 11 (build %u)", build);
  else if (major >= 10)
    std::snprintf(buf, sizeof(buf), "Windows 10 (build %u)", build);
  else
    std::snprintf(buf, sizeof(buf), "Windows %u.%u (build %u)", major, minor, build);
  return buf;
}

namespace {
// RtlGetVersion is unaffected by compatibility manifests (unlike GetVersionEx).
bool rtl_get_version(RTL_OSVERSIONINFOW* out) {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  if (!nt) return false;
  auto fn = (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion");
  if (!fn) return false;
  RTL_OSVERSIONINFOW vi = {sizeof(vi)};
  if (fn(&vi) != 0) return false;
  *out = vi;
  return true;
}
}  // namespace

std::wstring exe_dir() {
  wchar_t self[1024];
  DWORD n = GetModuleFileNameW(nullptr, self, 1024);
  if (n == 0 || n >= 1024) return L"";  // failure or truncated path
  std::wstring dir(self, n);
  size_t slash = dir.find_last_of(L'\\');
  if (slash == std::wstring::npos) return L"";
  return dir.substr(0, slash + 1);  // trailing backslash included
}

uint32_t windows_build() {
  RTL_OSVERSIONINFOW vi = {};
  return rtl_get_version(&vi) ? vi.dwBuildNumber : 0;
}

bool is_admin() {
  BOOL admin = FALSE;
  PSID sid = nullptr;
  SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
  if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,
                               0, 0, 0, 0, 0, &sid)) {
    CheckTokenMembership(nullptr, sid, &admin);
    FreeSid(sid);
  }
  return admin != FALSE;
}

bool service_exists(const char* name) {
  SC_HANDLE sc = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!sc) return false;
  SC_HANDLE svc = OpenServiceA(sc, name, SERVICE_QUERY_STATUS);
  bool ok = svc != nullptr;
  if (svc) CloseServiceHandle(svc);
  CloseServiceHandle(sc);
  return ok;
}

std::string find_usbip_exe() {
  const char* kCandidates[] = {
      "C:\\Program Files\\USBip\\usbip.exe",
      "C:\\Program Files (x86)\\USBip\\usbip.exe",
  };
  for (auto* p : kCandidates)
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return p;
  char pathbuf[MAX_PATH];
  DWORD n = SearchPathA(nullptr, "usbip.exe", nullptr, MAX_PATH, pathbuf, nullptr);
  if (n > 0 && n < MAX_PATH) return std::string(pathbuf, n);
  return "";
}

bool file_next_to_exe(const char* name) {
  std::wstring dir = exe_dir();
  if (dir.empty()) return false;  // fail closed — never probe the CWD instead
  return GetFileAttributesW((dir + pb::to_w(name)).c_str()) != INVALID_FILE_ATTRIBUTES;
}

namespace {
// Authoritative Bluetooth signals on an Enum\USB instance key. VID whitelists
// misfire on multi-product vendors (Realtek NICs, MediaTek Wi-Fi dongles), so
// classify by device class instead.
bool instance_is_bluetooth(HKEY ik) {
  auto sz_is = [&](const char* name, const char* expect) {
    char buf[128] = {};
    DWORD len = sizeof(buf) - 1, type = 0;  // -1: room to force NUL termination
    LONG rc = RegQueryValueExA(ik, name, nullptr, &type, (LPBYTE)buf, &len);
    if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) return false;
    buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
    return type == REG_SZ && _stricmp(buf, expect) == 0;
  };
  if (sz_is("Service", "BTHUSB") || sz_is("Class", "Bluetooth")) return true;
  // CompatibleIDs: query on the stack buffer; on overflow re-read once into a
  // heap buffer sized from the reported length. Only a fully successful read
  // is parsed (a second MORE_DATA means the value is still growing — parsing
  // a truncated list could silently drop the Class_E0 entry).
  char buf[1024];
  DWORD len = sizeof(buf) - 1, type = 0;
  LONG rc = RegQueryValueExA(ik, "CompatibleIDs", nullptr, &type, (LPBYTE)buf, &len);
  if (rc == ERROR_MORE_DATA) {
    std::vector<char> big(len + 1);
    DWORD need = (DWORD)big.size();
    if (RegQueryValueExA(ik, "CompatibleIDs", nullptr, &type, (LPBYTE)big.data(), &need) !=
        ERROR_SUCCESS)
      return false;
    big[big.size() - 1] = '\0';
    return type == REG_MULTI_SZ && has_class_e0_compatid(big.data());
  }
  if (rc != ERROR_SUCCESS) return false;
  buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
  return type == REG_MULTI_SZ && has_class_e0_compatid(buf);
}

// An Enum\USB device key stays present after the device is disabled in Device
// Manager, so key existence alone can never turn green again. An instance is
// "active" unless its ConfigFlags has CONFIGFLAG_DISABLED (0x1) set.
bool instance_active(HKEY ik) {
  DWORD flags = 0, size = sizeof(flags), type = 0;
  return !(RegQueryValueExA(ik, "ConfigFlags", nullptr, &type, (LPBYTE)&flags, &size) ==
               ERROR_SUCCESS &&
           type == REG_DWORD && (flags & 0x1));
}
}  // namespace

bool builtin_bluetooth_present(const std::string& our_vid) {
  HKEY key;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ,
                    &key) != ERROR_SUCCESS)
    return false;
  char sub[256];
  for (DWORD i = 0;; ++i) {
    DWORD len = sizeof(sub);
    if (RegEnumKeyExA(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
      break;
    std::string devkey(sub, len);
    if (!foreign_vid_enum_key(devkey, our_vid)) continue;
    size_t amp = devkey.find('&');
    bool vid_known_bt = known_bt_vid(devkey.substr(0, amp));  // second axis
    HKEY vk;
    if (RegOpenKeyExA(key, sub, 0, KEY_READ, &vk) != ERROR_SUCCESS) continue;
    char inst[256];
    bool found = false;
    for (DWORD j = 0; !found; ++j) {
      DWORD ilen = sizeof(inst);
      if (RegEnumKeyExA(vk, j, inst, &ilen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        break;
      HKEY ik;
      if (RegOpenKeyExA(vk, inst, 0, KEY_READ, &ik) != ERROR_SUCCESS) {
        // Fail conservative ONLY for known-BT vendors: a missed radio corrupts
        // acceptance; for unknown vendors an unreadable key (usually an unplug
        // race on some unrelated device) must not fake a BT warning.
        if (vid_known_bt) found = true;
        continue;
      }
      bool is_bt = instance_is_bluetooth(ik) || vid_known_bt;
      found = is_bt && instance_active(ik);
      RegCloseKey(ik);
    }
    RegCloseKey(vk);
    if (found) {
      RegCloseKey(key);
      return true;
    }
  }
  RegCloseKey(key);
  return false;
}

bool port_listened_by_other(uint16_t port) {
  // Query the system listener table for the owning PID: a listener owned by this
  // process (the GUI's embedded server) is not an "other" user of the port. A plain
  // connect() probe cannot tell who owns the port and would flag ourselves as a
  // second running instance.
  ULONG size = 0;
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
  if (size == 0) return false;
  std::vector<uint8_t> buf(size);
  if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
      NO_ERROR)
    return false;
  auto* table = (MIB_TCPTABLE_OWNER_PID*)buf.data();
  DWORD self = GetCurrentProcessId();
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto& row = table->table[i];
    if (row.dwLocalPort == 0 || row.dwOwningPid == self) continue;
    if (ntohs((u_short)row.dwLocalPort) == port) return true;
  }
  return false;
}

std::vector<CheckResult> run_checks() {
  std::vector<CheckResult> out;
  RTL_OSVERSIONINFOW vi = {};
  bool have_ver = rtl_get_version(&vi);
  std::string usbip_exe = find_usbip_exe();  // filesystem + PATH scan: do it once

  {  // 1) 操作系统版本
    CheckResult r;
    r.id = "os";
    r.title = "操作系统版本";
    uint32_t build = have_ver ? vi.dwBuildNumber : 0;
    r.detail = classify_os(build, have_ver ? vi.dwMajorVersion : 0,
                           have_ver ? vi.dwMinorVersion : 0);
    if (os_build_ok(build)) {
      r.status = Status::Ok;
    } else {
      r.status = Status::Fail;
      r.detail += " —— 需要 Windows 10 1903 或更高版本，请先升级系统。";
    }
    out.push_back(r);
  }
  {  // 2) usbip-win2 客户端工具
    CheckResult r;
    r.id = "usbip_exe";
    r.title = "USB 转发客户端（usbip-win2）";
    if (!usbip_exe.empty()) {
      r.status = Status::Ok;
      r.detail = usbip_exe;
    } else {
      r.status = Status::Fail;
      r.detail = "未安装。虚拟设备要挂载到本机 USB 总线，依赖 usbip-win2 客户端。点击右侧按钮前往官方发布页下载安装包，安装后无需重启即可检测到。";
      r.action_url = usbip_download_url();
      r.action_label = "打开 usbip-win2 下载页";
    }
    out.push_back(r);
  }
  {  // 3) usbip-win2 内核驱动服务
    CheckResult r;
    r.id = "usbip_driver";
    r.title = "内核驱动服务";
    if (!service_exists("usbip2_ude")) {
      if (usbip_exe.empty()) {
        r.status = Status::Warn;  // exe 都没装，驱动项并入上项提示
        r.detail = "（先安装 usbip-win2，驱动随安装包部署）";
      } else {
        r.status = Status::Fail;
        r.detail = "客户端已安装，但内核驱动服务缺失。请重新运行安装包，或以管理员身份执行其驱动安装脚本，完成后重启计算机再试。";
        r.action_url = usbip_docs_url();
        r.action_label = "查看驱动安装文档";
      }
    } else {
      r.status = Status::Ok;
      r.detail = "已安装。挂载设备时若被系统拒绝，请以管理员身份重新运行本程序。";
    }
    out.push_back(r);
  }
  {  // 4) 管理员权限
    CheckResult r;
    r.id = "admin";
    r.title = "管理员权限";
    if (is_admin()) {
      r.status = Status::Ok;
      r.detail = "已提升（可执行 attach / 驱动安装）";
    } else {
      r.status = Status::Warn;
      r.detail = "当前以普通权限运行：仿真与界面功能可用，但把虚拟设备挂载到系统、以及真机验收步骤需要管理员权限。请关闭本程序，右键选择「以管理员身份运行」重新打开。";
    }
    out.push_back(r);
  }
  {  // 5) winrt 探针
    CheckResult r;
    r.id = "probe";
    r.title = "验收测试探针";
    if (file_next_to_exe("winrt-probe.exe")) {
      r.status = Status::Ok;
      r.detail = "就绪";
    } else {
      r.status = Status::Warn;
      r.detail = "未在程序目录找到验收探针（winrt-probe.exe，四项里程碑的自动验收都需要它）。重新运行一次 build.bat 即会自动生成到同目录。";
    }
    out.push_back(r);
  }
  {  // 6) 内置蓝牙冲突（R-110）
    CheckResult r;
    r.id = "builtin_bt";
    r.title = "内置蓝牙适配器";
    if (builtin_bluetooth_present("VID_0B00")) {
      r.status = Status::Warn;
      r.detail = "检测到本机自带物理蓝牙适配器。真实与虚拟蓝牙并存可能干扰系统蓝牙界面与验收判定。建议在「设备管理器」中暂时禁用内置蓝牙（验收完成后可随时恢复）。";
    } else {
      r.status = Status::Ok;
      r.detail = "未检测到（或已全部禁用）";
    }
    out.push_back(r);
  }
  {  // 7) control port / stale instance
    CheckResult r;
    r.id = "port";
    r.title = "控制端口 3240";
    if (port_listened_by_other(3240)) {
      r.status = Status::Warn;
      r.detail = "端口已被其他程序占用，可能是另一个测试台实例正在运行。请先关闭旧实例（任务管理器中结束多余的 phantombt 进程），或重启计算机后再试。";
    } else {
      r.status = Status::Ok;
      r.detail = "可用（本程序监听不计入占用）";
    }
    out.push_back(r);
  }
  return out;
}

bool needs_attention(const std::vector<CheckResult>& checks) {
  for (auto& c : checks)
    if (c.status != Status::Ok && c.status != Status::Info) return true;
  return false;
}

bool first_run_done() {
  HKEY key;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\PhantomBT", 0, KEY_READ, &key) != ERROR_SUCCESS)
    return false;
  DWORD v = 0, size = sizeof(v), type = 0;
  bool done = RegQueryValueExA(key, "FirstRunDone", nullptr, &type, (LPBYTE)&v, &size) ==
                  ERROR_SUCCESS &&
              v == 1;
  RegCloseKey(key);
  return done;
}

void mark_first_run() {
  HKEY key;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\PhantomBT", 0, nullptr, 0, KEY_WRITE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS)
    return;
  DWORD v = 1;
  RegSetValueExA(key, "FirstRunDone", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
  RegCloseKey(key);
}

}  // namespace pb::env
