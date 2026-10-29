// winrt-probe.exe：WinRT 验收探针
// 模式：
//   hid      --vid 0x1209 --pid 0xB00D --usage-page 0xFF00 --usage 0x01
//            [--expect-hex <prefix>] [--wait-ms 5000]
//   radio    [--wait-ms 15000]                     （M1）
//   watcher  --expect-name <name> [--duration-ms 8000]   （M2）
//   ble      --address <hex12> [--expect-service 1812] ... （M3）
// 输出一行 JSON；退出码 0=pass 2=fail 3=skip
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <windows.h>

using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::HumanInterfaceDevice;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace {

using Args = std::map<std::string, std::string>;

Args parse_args(int argc, char** argv, int skip) {
  Args a;
  for (int i = skip; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
  return a;
}

uint32_t num(const Args& a, const char* k, uint32_t def) {
  auto it = a.find(k);
  if (it == a.end()) return def;
  const std::string& s = it->second;
  return s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0 ? std::stoul(s, nullptr, 16)
                                                        : std::stoul(s, nullptr, 10);
}
std::string str(const Args& a, const char* k, const std::string& def = "") {
  auto it = a.find(k);
  return it == a.end() ? def : it->second;
}

std::string to_hex(const std::vector<uint8_t>& v) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (uint8_t b : v) {
    s.push_back(d[b >> 4]);
    s.push_back(d[b & 0xF]);
  }
  return s;
}

int finish(bool pass, const std::string& reason, const std::vector<std::string>& reports) {
  std::string json = "{\"result\":\"";
  json += pass ? "pass" : "fail";
  json += "\"";
  if (!reason.empty()) json += ",\"reason\":\"" + reason + "\"";
  if (!reports.empty()) {
    json += ",\"reports\":[";
    for (size_t i = 0; i < reports.size(); ++i) {
      if (i) json += ",";
      json += "\"" + reports[i] + "\"";
    }
    json += "]";
  }
  json += "}";
  std::printf("%s\n", json.c_str());
  return pass ? 0 : 2;
}

int cmd_hid(const Args& a) {
  uint16_t vid = (uint16_t)num(a, "--vid", 0);
  uint16_t pid = (uint16_t)num(a, "--pid", 0);
  uint16_t usage_page = (uint16_t)num(a, "--usage-page", 0xFF00);
  uint16_t usage = (uint16_t)num(a, "--usage", 0x01);
  uint32_t wait_ms = num(a, "--wait-ms", 5000);
  std::string expect = str(a, "--expect-hex");

  try {
    auto selector = HidDevice::GetDeviceSelector(usage_page, usage, vid, pid);
    auto devices = DeviceInformation::FindAllAsync(selector).get();
    if (devices.Size() == 0) return finish(false, "nodev", {});
    auto hid = HidDevice::FromIdAsync(devices.GetAt(0).Id(), FileAccessMode::ReadWrite).get();
    if (!hid) return finish(false, "open", {});

    std::mutex mx;
    std::vector<std::string> reports;
    std::atomic<bool> matched{false};

    auto token = hid.InputReportReceived([&](HidDevice const&, HidInputReportReceivedEventArgs const& args) {
      auto data = args.Report().Data();
      DataReader reader = DataReader::FromBuffer(data);
      std::vector<uint8_t> bytes(data.Length(), 0);
      if (!bytes.empty()) reader.ReadBytes(winrt::array_view<uint8_t>(bytes));
      std::string hex = to_hex(bytes);
      {
        std::lock_guard<std::mutex> lk(mx);
        reports.push_back(hex);
      }
      if (!expect.empty() && hex.rfind(expect, 0) == 0) matched = true;
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (matched.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    hid.InputReportReceived(token);
    hid.Close();

    std::lock_guard<std::mutex> lk(mx);
    if (!expect.empty() && !matched.load()) return finish(false, "noreport", reports);
    if (reports.empty()) return finish(false, "noreport", reports);
    return finish(true, "", reports);
  } catch (const winrt::hresult_error& e) {
    return finish(false, winrt::to_string(e.message()), {});
  } catch (const std::exception& e) {
    return finish(false, e.what(), {});
  }
}

int cmd_radio_winrt(const Args& a) {
  (void)a;
  try {
    auto radios = Windows::Devices::Radios::Radio::GetRadiosAsync().get();
    for (auto const& r : radios) {
      if (r.Kind() == Windows::Devices::Radios::RadioKind::Bluetooth) {
        bool on = r.State() == Windows::Devices::Radios::RadioState::On;
        if (on) return finish(true, "name:" + winrt::to_string(r.Name()), {});
        return finish(false, "state:" + std::to_string((int32_t)r.State()), {});
      }
    }
    return finish(false, "noradio", {});
  } catch (const winrt::hresult_error& e) {
    return finish(false, winrt::to_string(e.message()), {});
  }
}

// 降级路径：查注册表判断 USB 蓝牙适配器驱动绑定（无需 WinRT）
int cmd_radio_registry(const Args& a) {
  (void)a;
  HKEY key;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return finish(false, "no-usb-enum-key", {});
  }
  char sub[256];
  DWORD i = 0;
  for (; i < 4096; ++i) {
    DWORD len = sizeof(sub);
    if (RegEnumKeyExA(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
      break;
    // VID_0B00&PID_0001 = phantom-bt 适配器
    if (std::string(sub, len).rfind("VID_0B00&PID_0001", 0) == 0) {
      RegCloseKey(key);
      return finish(true, "driver-bound:" + std::string(sub, len), {});
    }
  }
  RegCloseKey(key);
  return finish(false, "nodev", {});
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: winrt-probe <hid|radio|watcher|ble> [--key value ...]\n");
    return 3;
  }
  std::string mode = argv[1];
  Args a = parse_args(argc, argv, 2);
  try {
    if (mode == "hid") return cmd_hid(a);
    if (mode == "radio") return cmd_radio_winrt(a);
    if (mode == "radio-registry") return cmd_radio_registry(a);
    std::fprintf(stderr, "winrt-probe: mode '%s' not implemented yet\n", mode.c_str());
    return 3;
  } catch (const std::exception& e) {
    return finish(false, e.what(), {});
  }
}
