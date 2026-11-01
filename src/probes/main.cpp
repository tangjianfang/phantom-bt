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
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
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
    // 设备发现带重试：attach 后 Windows 枚举+驱动启动需要数秒，wait_ms 同时覆盖
    // 发现与报告等待两个阶段
    auto selector = HidDevice::GetDeviceSelector(usage_page, usage, vid, pid);
    auto discover_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    winrt::Windows::Devices::Enumeration::DeviceInformationCollection devices = nullptr;
    for (;;) {
      devices = DeviceInformation::FindAllAsync(selector).get();
      if (devices.Size() > 0) break;
      if (std::chrono::steady_clock::now() >= discover_deadline) return finish(false, "nodev", {});
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
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
    // R-110：多 radio 共存时"取第一个 Bluetooth radio"会拿到内置物理 radio
    // （如 Intel）——物理 radio 开着时是假 pass。必须优先匹配虚拟 radio 的
    // 名字（adapter_model.cpp 的 product = "Phantom-BT Radio"）。
    std::vector<winrt::Windows::Devices::Radios::Radio> bt;
    for (auto const& r : radios)
      if (r.Kind() == Windows::Devices::Radios::RadioKind::Bluetooth) bt.push_back(r);
    if (bt.empty()) return finish(false, "noradio", {});
    for (auto const& r : bt) {
      if (winrt::to_string(r.Name()) == "Phantom-BT Radio") {
        bool on = r.State() == Windows::Devices::Radios::RadioState::On;
        if (on) return finish(true, "name:" + winrt::to_string(r.Name()), {});
        return finish(false, "state:" + std::to_string((int32_t)r.State()), {});
      }
    }
    // 虚拟 radio 未枚举到：其它 Bluetooth radio（物理）再 On 也不算过——
    // 这正是本探针要抓的"BTHPORT 未 onboard 到虚拟适配器"状态
    return finish(false, "nophantomradio bt:" + std::to_string(bt.size()), {});
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

int cmd_watcher(const Args& a) {
  uint32_t duration = num(a, "--duration-ms", 8000);
  std::string expect_name = str(a, "--expect-name");
  try {
    Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(
        Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode::Active);
    std::mutex mx;
    std::vector<std::pair<uint64_t, std::string>> seen;
    std::atomic<bool> matched{false};
    watcher.Received([&](auto const&, auto const& args) {
      auto adv = args.Advertisement();
      std::string name = winrt::to_string(adv.LocalName());
      uint64_t addr = args.BluetoothAddress();
      {
        std::lock_guard<std::mutex> lk(mx);
        seen.emplace_back(addr, name);
      }
      if (!expect_name.empty() && name.find(expect_name) != std::string::npos) matched = true;
    });
    watcher.Start();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
    while (std::chrono::steady_clock::now() < deadline) {
      if (!expect_name.empty() && matched.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    watcher.Stop();
    std::lock_guard<std::mutex> lk(mx);
    if (!expect_name.empty() && !matched.load()) return finish(false, "noname", {});
    if (seen.empty()) return finish(false, "noseen", {});
    std::vector<std::string> reports;
    for (auto& [addr, name] : seen) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%012llX %s", (unsigned long long)addr, name.c_str());
      reports.push_back(buf);
    }
    return finish(true, "", reports);
  } catch (const winrt::hresult_error& e) {
    return finish(false, winrt::to_string(e.message()), {});
  }
}

int cmd_ble(const Args& a) {
  std::string addr_hex = str(a, "--address");  // 12 hex digits, MSB first
  uint32_t wait_ms = num(a, "--wait-ms", 15000);
  std::string expect_service = str(a, "--expect-service", "1812");
  if (addr_hex.size() != 12) return finish(false, "bad address", {});
  uint64_t addr = 0;
  for (char c : addr_hex) addr = (addr << 4) | (uint64_t)(std::stoul(std::string(1, c), nullptr, 16));
  try {
    auto device = Windows::Devices::Bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
    if (!device) return finish(false, "nodev", {});
    auto services_result = device.GetGattServicesAsync(
                                Windows::Devices::Bluetooth::BluetoothCacheMode::Uncached)
                                .get();
    if (services_result.Status() !=
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus::Success) {
      return finish(false, "gatt-status", {});
    }
    std::vector<std::string> found;
    bool has_hid = false;
    for (auto const& svc : services_result.Services()) {
      char buf[8];
      uint16_t uuid = (uint16_t)(svc.Uuid().Data1 & 0xFFFF);
      std::snprintf(buf, sizeof(buf), "%04x", uuid);
      found.push_back(buf);
      if (uuid == 0x1812) has_hid = true;
    }
    std::string reason = has_hid ? "" : "no-hid-service";
    // HID 设备节点由 BthLEEnum 异步生成；等待 HID 集合出现（usagePage 1 usage 6 键盘）
    bool hid_opened = false;
    if (has_hid) {
      auto selector = Windows::Devices::HumanInterfaceDevice::HidDevice::GetDeviceSelector(1, 6);
      for (uint32_t i = 0; i < wait_ms / 500 && !hid_opened; ++i) {
        auto devices = Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector).get();
        if (devices.Size() > 0) {
          hid_opened = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    device.Close();
    if (!has_hid) return finish(false, reason, found);
    if (!hid_opened) return finish(false, "no-hid-node", found);
    return finish(true, "", found);
  } catch (const winrt::hresult_error& e) {
    return finish(false, winrt::to_string(e.message()), {});
  }
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
    if (mode == "watcher") return cmd_watcher(a);
    if (mode == "ble") return cmd_ble(a);
    std::fprintf(stderr, "winrt-probe: mode '%s' not implemented yet\n", mode.c_str());
    return 3;
  } catch (const std::exception& e) {
    return finish(false, e.what(), {});
  }
}
