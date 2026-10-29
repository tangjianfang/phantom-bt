# Phantom-BT 实施计划（M-1 ~ M4）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 上纯软件仿真 USB HID 设备、USB 蓝牙适配器与 BLE HID 外设，使 WinRT API（HidDevice / BluetoothLEAdvertisementWatcher / BluetoothLEDevice）可见可用，提供 CLI + Win32 GUI 与全自动化测试。

**Architecture:** 用户态 C++ USB/IP 服务器经 TCP:3240 对接预装的 usbip-win2 (UDE) 内核驱动；USB 设备核心之上挂 HID 模型与蓝牙适配器模型；适配器模型对接进程内自研 MiniController（HCI 子集 + LE 空口队列），虚拟 BLE 外设（ATT/GATT/HOGP/SMP 应答端）与 Windows 主机栈在同一进程内直连；控制层被 CLI/GUI/命名管道共享。

**Tech Stack:** C++20 / MSVC (VS 18) / CMake≥3.25 + Ninja / Winsock2 / Win32 API / C++/WinRT（仅探针）/ 自研零依赖测试框架 / OpenCppCoverage。

## Global Constraints

- 平台：Windows 10 1903+ x64；构建机 CMake 4.2、Ninja 1.13、VS 18 Community（`C:\Program Files\Microsoft Visual Studio\18\Community`）。
- 零第三方运行时依赖：仅 Win32/Winsock/WinRT SDK；usbip-win2 为外部预装驱动（BSD-2-Clause），不属于本仓库代码。
- 命名：命名空间 `pb`；文件/目录 snake_case；类型/函数 PascalCase；宏 `PB_` 前缀。
- 字节序：USB/IP 线上字段大端；USB 描述符多字节字段小端；HCI/ATT 多字节字段小端。
- 编译：`/utf-8 /W4 /permissive-`，异常开启，C++20。
- 每任务：先写失败测试 → 实现 → 测试全绿 → Conventional Commits 提交。
- 单测命令：`build/tests/phantombt-tests.exe [filter]`（filter 为测试名子串）。
- 覆盖率口径：`src/core src/usbip src/usb src/hid src/hci src/ble src/control src/sim` 行覆盖 100%（OpenCppCoverage）；`gui/probes/cli` 用冒烟+集成测试覆盖，不进 100% 指标。
- 集成脚本（需管理员 + usbip-win2 驱动）无权限时以退出码 3 (SKIP) 优雅跳过，不判 FAIL。
- 本机未装 WSL；RootCanal 路线弃用，禁止引入 WSL 依赖。

## 执行环境注意（给执行者）

- 本会话可能无管理员权限：Task 13/17/19/24 的 `run-m*.ps1` 真机段需管理员；若 `usbip.exe` 不存在或无权限，标记 SKIP 并在最终报告列人工步骤，不视为任务失败。回环集成测试（不经内核驱动）必须全部可跑。
- 全部任务在仓库根目录直接执行（干净 main 分支）。

---

## M-1 工程底座

### Task 1: 构建骨架

**Files:**
- Create: `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `tools/build.ps1`, `src/core/version.h.in`, `tests/main.cpp`

**Interfaces:**
- Produces: 目标 `phantombt-tests`（空测试可执行）；构建命令 `tools/build.ps1 [-Config Debug|Release]`。

- [ ] **Step 1: 写根 CMakeLists 与预设**

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.25)
project(phantom-bt LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
  add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
endif()
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
```

`src/CMakeLists.txt`（初始为空库占位，后续任务追加）:
```cmake
add_library(phantom_core STATIC)
target_include_directories(phantom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

`tests/CMakeLists.txt`:
```cmake
add_executable(phantombt-tests main.cpp)
target_link_libraries(phantombt-tests PRIVATE phantom_core)
```

`tests/main.cpp`（临时，Task 2 替换为框架宏）:
```cpp
int main() { return 0; }
```

`CMakePresets.json`:
```json
{
  "version": 3,
  "configurePresets": [
    { "name": "msvc-debug", "generator": "Ninja", "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "msvc-release", "generator": "Ninja", "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } }
  ],
  "buildPresets": [
    { "name": "debug", "configurePreset": "msvc-debug" },
    { "name": "release", "configurePreset": "msvc-release" }
  ]
}
```

`.gitignore`: `build/`, `*.user`, `.vs/`, `integration-results/`

- [ ] **Step 2: 写构建脚本 tools/build.ps1**

用 vswhere 定位 VsDevCmd 并初始化 MSVC 环境，再 configure + build：
```powershell
param([string]$Config = "Debug")
$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -property installationPath
$devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
cmd /c "`"$devCmd`" -arch=x64 -no_logo && cmake --preset msvc-$($Config.ToLower()) && cmake --build --preset $($Config.ToLower())" | Tee-Object -FilePath build.log
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

- [ ] **Step 3: 构建并验证**

Run: `powershell -File tools/build.ps1`
Expected: `build\debug\tests\phantombt-tests.exe` 生成且运行退出码 0。

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "build: CMake+Ninja 骨架与构建脚本"
```

### Task 2: 测试框架

**Files:**
- Create: `src/test/test_framework.h`, `tests/main.cpp`（替换）, `tests/unit/test_smoke.cpp`

**Interfaces:**
- Produces: `PB_TEST(name)` 注册测试；`PB_CHECK(c)` / `PB_CHECK_EQ(a,b)` / `PB_CHECK_THROWS(e)` 断言；`PB_TEST_MAIN()` 生成 main；`pb::TestRegistry::instance().run_all(filter)` 执行；非零退出码表示有失败。

- [ ] **Step 1: 写冒烟失败测试**

`tests/unit/test_smoke.cpp`:
```cpp
#include "test/test_framework.h"
PB_TEST(smoke_framework_registers) {
  pb_t.checks = pb_t.checks; // framework provides ctx
  PB_CHECK_EQ(1 + 1, 2);
}
PB_TEST(smoke_failure_demo_disabled) { /* 保留空：验证注册计数 */ }
```

`tests/main.cpp`:
```cpp
#include "test/test_framework.h"
PB_TEST_MAIN()
```

`tests/CMakeLists.txt` 追加源：`add_executable(phantombt-tests main.cpp unit/test_smoke.cpp)`

- [ ] **Step 2: 运行确认失败**

Run: `powershell -File tools/build.ps1; build\debug\tests\phantombt-tests.exe`
Expected: 编译失败（`test/test_framework.h` 不存在）。

- [ ] **Step 3: 实现框架**

`src/test/test_framework.h`:
```cpp
#pragma once
#include <exception>
#include <cstdio>
#include <string>
#include <vector>
namespace pb {
struct TestCtx {
  int checks = 0, failures = 0;
  const char* name = "";
  void fail(const char* what, const char* file, int line) {
    ++failures;
    std::printf("  FAIL [%s] %s (%s:%d)\n", name, what, file, line);
  }
};
using TestFn = void (*)(TestCtx&);
class TestRegistry {
 public:
  static TestRegistry& instance() { static TestRegistry r; return r; }
  void add(const char* name, TestFn fn) { tests_.push_back({name, fn}); }
  int run_all(const std::string& filter) {
    int failed_tests = 0, total_run = 0;
    for (auto& t : tests_) {
      if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
      ++total_run;
      TestCtx ctx; ctx.name = t.name;
      try { t.fn(ctx); } catch (const std::exception& e) { ctx.fail(e.what(), __FILE__, __LINE__); }
      if (ctx.failures) { ++failed_tests; std::printf("[%s] %d check(s) FAILED\n", t.name, ctx.failures); }
      else std::printf("[%s] ok (%d checks)\n", t.name, ctx.checks);
    }
    std::printf("== %d/%d tests passed ==\n", total_run - failed_tests, total_run);
    return failed_tests ? 1 : 0;
  }
 private:
  struct Entry { const char* name; TestFn fn; };
  std::vector<Entry> tests_;
};
}
#define PB_TEST(name) \
  static void pb_test_fn_##name(pb::TestCtx& pb_t); \
  static bool pb_reg_##name = (pb::TestRegistry::instance().add(#name, &pb_test_fn_##name), true); \
  static void pb_test_fn_##name(pb::TestCtx& pb_t)
#define PB_CHECK(c) do { ++pb_t.checks; if (!(c)) pb_t.fail(#c, __FILE__, __LINE__); } while (0)
#define PB_CHECK_EQ(a, b) do { ++pb_t.checks; if (!((a) == (b))) { std::string m = #a " != " #b; pb_t.fail(m.c_str(), __FILE__, __LINE__); } } while (0)
#define PB_CHECK_THROWS(e) do { ++pb_t.checks; bool threw_ = false; try { (void)(e); } catch (...) { threw_ = true; } if (!threw_) pb_t.fail(#e " throws", __FILE__, __LINE__); } while (0)
#define PB_TEST_MAIN() int main(int argc, char** argv) { return pb::TestRegistry::instance().run_all(argc > 1 ? argv[1] : ""); }
```

- [ ] **Step 4: 运行确认通过**

Run: `powershell -File tools/build.ps1; build\debug\tests\phantombt-tests.exe`
Expected: `== 2/2 tests passed ==`，退出码 0。

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "test: 零依赖测试框架"
```

### Task 3: core/bytes

**Files:**
- Create: `src/core/bytes.h`, `src/core/bytes.cpp`
- Test: `tests/unit/test_bytes.cpp`

**Interfaces:**
- Produces:
```cpp
namespace pb {
class ByteWriter {  // 追加写，目标小端为主，USB/IP 用 be 系列
 public:
  void u8(uint8_t v); void u16le(uint16_t v); void u16be(uint16_t v);
  void u32le(uint32_t v); void u32be(uint32_t v); void u64be(uint64_t v);
  void bytes(const uint8_t* p, size_t n); void zeros(size_t n);
  const std::vector<uint8_t>& buf() const; std::vector<uint8_t> take(); size_t size() const;
};
class ByteReader { // 越界后进入 error 态，后续读取返回 0
 public:
  ByteReader(const uint8_t* p, size_t n);
  uint8_t u8(); uint16_t u16le(); uint16_t u16be(); uint32_t u32le(); uint32_t u32be(); uint64_t u64be();
  std::vector<uint8_t> bytes(size_t n); void skip(size_t n);
  bool ok() const; size_t remaining() const; const uint8_t* cur() const;
};
std::string to_hex(const uint8_t* p, size_t n);              // 小写无分隔
std::string to_hex(const std::vector<uint8_t>& v);
std::vector<uint8_t> from_hex(std::string_view s);           // 偶数长度、仅 0-9a-fA-F，否则抛 std::runtime_error
}
```

- [ ] **Step 1: 写失败测试** `tests/unit/test_bytes.cpp`：

```cpp
#include "test/test_framework.h"
#include "core/bytes.h"
PB_TEST(bytes_writer_endian) {
  pb::ByteWriter w;
  w.u16le(0x1234); w.u16be(0x1234); w.u32be(0x01020304);
  PB_CHECK_EQ(pb::to_hex(w.buf()), std::string("3412123401020304"));
}
PB_TEST(bytes_reader_overrun_sets_error) {
  uint8_t d[] = {1, 2};
  pb::ByteReader r(d, 2);
  PB_CHECK_EQ(r.u16be(), 0x0102);
  PB_CHECK(!r.ok());
  PB_CHECK_EQ(r.remaining(), 0u);
}
PB_TEST(bytes_from_hex_rejects_odd) {
  PB_CHECK_THROWS(pb::from_hex(\"abc\"));
  auto v = pb::from_hex("0a0B");
  PB_CHECK_EQ(v.size(), 2u); PB_CHECK_EQ(v[1], 0x0B);
}
```
（追加到 `tests/CMakeLists.txt` 源列表。）

- [ ] **Step 2: 构建** Run: `powershell -File tools/build.ps1` — Expected: 编译失败（core/bytes.h 不存在）。

- [ ] **Step 3: 实现 bytes.h/.cpp**（接口如上，行为以测试为准；`from_hex` 允许大写输入）

- [ ] **Step 4: 运行** Run: `build\debug\tests\phantombt-tests.exe bytes` — Expected: 3/3 passed。

- [ ] **Step 5: Commit** `git add -A && git commit -m "feat(core): 字节读写与hex工具"`

### Task 4: core/log

**Files:**
- Create: `src/core/log.h`, `src/core/log.cpp`
- Test: `tests/unit/test_log.cpp`

**Interfaces:**
- Produces:
```cpp
namespace pb {
enum class LogLevel : int { Debug = 0, Info, Warn, Error };
void log_set_level(LogLevel lv);                       // 默认 Info
void log_set_sink(std::function<void(LogLevel, std::string_view line)>); // 替换默认 stderr
void log_write(LogLevel lv, std::string_view msg);     // 行格式: [LEVEL][HH:MM:SS.mmm] msg\n
}
#define PB_LOG(lv, msg) do { if ((int)(lv) >= (int)pb::LogLevel::Debug) pb::log_write((lv), (msg)); } while (0)
// 便捷宏：PB_LOG_INFO / PB_LOG_WARN / PB_LOG_ERROR / PB_LOG_DEBUG 同构
```

- [ ] **Step 1: 失败测试** `tests/unit/test_log.cpp`：
```cpp
#include "test/test_framework.h"
#include "core/log.h"
#include <string>
#include <vector>
PB_TEST(log_sink_receives_lines) {
  std::vector<std::string> lines;
  pb::log_set_sink([&](pb::LogLevel lv, std::string_view s) { lines.push_back(std::string(s)); });
  pb::log_set_level(pb::LogLevel::Debug);
  PB_LOG_INFO("hello");
  PB_LOG_DEBUG("dbg");
  pb::log_set_sink(nullptr); // 恢复默认
  PB_CHECK_EQ(lines.size(), 2u);
  PB_CHECK(lines[1].find("dbg") != std::string::npos);
  PB_CHECK(lines[0].find("hello") != std::string::npos);
}
```
- [ ] **Step 2: 构建**（失败）
- [ ] **Step 3: 实现**（互斥保护 sink；时间用 `GetLocalTime`；sink 为空时写 stderr）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe log` — 1/1 passed
- [ ] **Step 5: Commit** `git commit -am "feat(core): 分级日志"`

### Task 5: core/net（Winsock 封装）

**Files:**
- Create: `src/core/net.h`, `src/core/net.cpp`
- Test: `tests/unit/test_net.cpp`

**Interfaces:**
- Produces:
```cpp
namespace pb::net {
struct WsaSession { WsaSession(); ~WsaSession(); };  // 引用计数式 WSAStartup/WSACleanup
class TcpConn {
 public:
  TcpConn() = default;  ~TcpConn(); TcpConn(TcpConn&&) noexcept; TcpConn& operator=(TcpConn&&) noexcept;
  static TcpConn connect(const std::string& host, uint16_t port, std::string* err);
  bool send_all(const uint8_t* p, size_t n, std::string* err);
  bool recv_exact(uint8_t* p, size_t n, std::string* err);
  bool recv_some(uint8_t* buf, size_t cap, int* n_out, std::string* err); // 阻塞，n_out=0 对端关闭
  void set_nodelay(bool on); void close(); bool is_open() const; uintptr_t raw() const; // raw=SOCKET
 private: uintptr_t s_ = ~0ull;
};
class TcpListener {
 public:
  bool listen(uint16_t port, std::string* err);        // 0.0.0.0:port，SO_REUSEADDR
  TcpConn accept(std::string* err);                    // 阻塞；失败时 TcpConn 未打开
  uint16_t port() const;
 private: uintptr_t s_ = ~0ull; uint16_t port_ = 0;
};
}
```

- [ ] **Step 1: 失败测试**（真实回环 socket 测试）`tests/unit/test_net.cpp`：
```cpp
#include "test/test_framework.h"
#include "core/net.h"
#include <thread>
PB_TEST(net_loopback_send_recv) {
  pb::net::WsaSession wsa;
  pb::net::TcpListener l;
  std::string err;
  PB_CHECK(l.listen(0, &err));              // port 0 = 系统分配
  std::thread srv([&] {
    std::string e2; auto c = l.accept(&e2);
    uint8_t buf[5]; c.recv_exact(buf, 5, &e2);
    const char reply[] = "pong!";
    c.send_all((const uint8_t*)reply, 5, &e2);
  });
  auto c = pb::net::TcpConn::connect("127.0.0.1", l.port(), &err);
  PB_CHECK(c.is_open());
  PB_CHECK(c.send_all((const uint8_t*)"ping!", 5, &err));
  uint8_t buf[5] = {};
  PB_CHECK(c.recv_exact(buf, 5, &err));
  PB_CHECK_EQ(std::string((char*)buf, 5), std::string("pong!"));
  srv.join();
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（`src/CMakeLists.txt` 中 `target_link_libraries(phantom_core PUBLIC ws2_32)`；所有错误经 `WSAGetLastError` 格式化进 err）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe net` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(core): Winsock TCP 封装"`

### Task 6: core/json（Profile 加载）

**Files:**
- Create: `src/core/json.h`, `src/core/json.cpp`
- Test: `tests/unit/test_json.cpp`

**Interfaces:**
- Produces:
```cpp
namespace pb::json {
struct Value {
  enum Type { Null, Bool, Number, String, Array, Object };
  Type type = Null; bool b = false; double num = 0; std::string str;
  std::vector<Value> arr; std::map<std::string, Value> obj;
  const Value* find(const char* key) const;       // 仅 Object，无则 nullptr
  std::string as_str() const; double as_num() const; bool as_bool() const; int as_int() const;
};
Value parse(std::string_view text);  // 抛 std::runtime_error("json: 行列信息+原因")
}
```

- [ ] **Step 1: 失败测试** `tests/unit/test_json.cpp`：
```cpp
#include "test/test_framework.h"
#include "core/json.h"
PB_TEST(json_object_nested_array) {
  auto v = pb::json::parse(R"({"template":"gamepad","vid":"0x1234","axes":[8,16],"flags":true})");
  PB_CHECK_EQ(v.find("template")->as_str(), std::string("gamepad"));
  PB_CHECK_EQ(v.find("axes")->arr.size(), 2u);
  PB_CHECK_EQ(v.find("axes")->arr[1].as_num(), 16.0);
  PB_CHECK(v.find("flags")->as_bool());
  PB_CHECK(v.find("missing") == nullptr);
}
PB_TEST(json_rejects_garbage) { PB_CHECK_THROWS(pb::json::parse("{ nope")); }
PB_TEST(json_escapes) {
  auto v = pb::json::parse("{\"s\":\"a\\\"b\\\\c\\n\"}");
  PB_CHECK_EQ(v.find("s")->as_str(), std::string("a\"b\\c\n"));
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（递归下降解析器，支持完整 JSON：数字含负号/小数、字符串 \uXXXX 转基本多文种、bool/null；输出位置错误；~500 行内）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe json` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(core): 极简 JSON 解析"`

---

## M0 USB HID

### Task 7: usbip 协议编解码

**Files:**
- Create: `src/usbip/protocol.h`, `src/usbip/protocol.cpp`
- Test: `tests/unit/test_usbip_protocol.cpp`

**Interfaces（常量与函数——后续所有任务依赖）：**
```cpp
namespace pb::usbip {
// 控制/枚举 opcodes（BE on wire）
constexpr uint32_t OP_REQ_DEVLIST = 0x80000000u;
constexpr uint32_t OP_REP_DEVLIST = 0x00000001u;
constexpr uint32_t OP_REQ_IMPORT  = 0x80000003u;
constexpr uint32_t OP_REP_IMPORT  = 0x00000004u;
// URB opcodes
constexpr uint32_t USBIP_CMD_SUBMIT = 0x00000001u;
constexpr uint32_t USBIP_CMD_UNLINK = 0x00000002u;
constexpr uint32_t USBIP_RET_SUBMIT = 0x00000003u;
constexpr uint32_t USBIP_RET_UNLINK = 0x00000004u;
constexpr uint32_t DIR_OUT = 0, DIR_IN = 1;
constexpr uint32_t USBIP_SPEED_FULL = 2;   // 12Mbps；devlist speed 字段

struct ExportedInterface { uint8_t iclass, isubclass, iprotocol; };
struct ExportedDevice {   // 与 usbip_usb_device 结构一一对应
  std::string path;       // /sys 风格唯一路径，如 "7-1"
  std::string busid;      // "7-1"
  uint32_t busnum = 7, devnum = 1, speed = USBIP_SPEED_FULL;
  uint16_t vid = 0, pid = 0;
  uint8_t dev_class = 0, dev_subclass = 0, dev_protocol = 0;
  uint8_t config_value = 1, num_configs = 1, num_interfaces = 1;
  std::vector<ExportedInterface> interfaces;
};
struct SubmitHeader {      // 解析后的 cmd_submit
  uint32_t command, seqnum, devid, direction, ep;
  uint32_t transfer_flags; int32_t transfer_buffer_length;
  int32_t start_frame; uint32_t number_of_packets; int32_t interval;
  uint8_t setup[8];
};
// 打包
std::vector<uint8_t> pack_op_header(uint32_t op, int32_t status);   // 4B op + 4B status（枚举应答通用）
std::vector<uint8_t> pack_rep_devlist(const std::vector<ExportedDevice>& devs);
std::vector<uint8_t> pack_rep_import(const ExportedDevice& d);
std::vector<uint8_t> pack_ret_submit(uint32_t seqnum, uint32_t devid, uint32_t ep, uint32_t direction,
                                     int32_t status, int32_t actual_length,
                                     const uint8_t* data, size_t data_len); // 40B头+data(仅IN)
std::vector<uint8_t> pack_ret_unlink(uint32_t seqnum, uint32_t devid, uint32_t ep, int32_t status);
// 解析
bool parse_op(uint32_t be_word);            // 无意义（占位防误用），忽略
bool parse_submit(const uint8_t* p, size_t n, SubmitHeader& out);   // 48B；false=长度不足
bool parse_unlink(const uint8_t* p, size_t n, uint32_t& seqnum, uint32_t& unlink_seqnum); // 24B
}
```

**字节布局（实现依据，来自 Linux 内核 `drivers/usb/usbip/usbip_common.h`，全部大端）：**
- basic: `u32 command, seqnum, devid, direction, ep`（20B）
- cmd_submit 追加：`u32 transfer_flags; s32 transfer_buffer_length; s32 start_frame; u32 number_of_packets; s32 interval; u8 setup[8]`（28B，共 48B）
- ret_submit：basic + `s32 status; s32 actual_length; s32 start_frame; s32 number_of_packets; s32 error_count`（40B）+ data（仅 direction==IN 且 actual_length>0）
- cmd_unlink：basic + `u32 unlink_seqnum`（24B）；ret_unlink：basic + `s32 status`（24B）
- rep_devlist：`u32 OP_REP_DEVLIST; u32 status=0; u32 ndev` + 每设备 `usbip_usb_device{char path[256]; char busid[32]; u32 busnum,devnum,speed; s32 vendor,product; u8 devClass,devSubclass,devProtocol; u8 inum,configurations,numinterfaces}` 注意内核结构为 `s32 vendor`（有符号但按无符号值传输）——按 u32 写 idVendor/idProduct；随后每接口 `usbip_usb_interface{u8 bInterfaceClass,bInterfaceSubClass,bInterfaceProtocol;padding}`（4B）
- rep_import：`u32 OP_REP_IMPORT; u32 status=0` + usbip_usb_device（**不含**接口数组）
- rep 错误形态：`u32 OP_REP_IMPORT; u32 status!=0`（如 1=节点不存在）

- [ ] **Step 1: 失败测试** `tests/unit/test_usbip_protocol.cpp`：
```cpp
#include "test/test_framework.h"
#include "usbip/protocol.h"
using namespace pb::usbip;
static ExportedDevice sample() {
  ExportedDevice d; d.path = "/phantom/7-1"; d.busid = "7-1";
  d.vid = 0x1234; d.pid = 0x5678; d.dev_class = 0; d.num_interfaces = 1;
  d.interfaces.push_back({3, 0, 0});
  return d;
}
PB_TEST(usbip_rep_devlist_layout) {
  auto b = pack_rep_devlist({sample()});
  PB_CHECK_EQ(b.size(), 12u + 312u + 4u);              // 头12 + 设备312 + 接口4
  PB_CHECK_EQ(b[0], 0x00); PB_CHECK_EQ(b[3], 0x01);    // OP_REP_DEVLIST BE
  PB_CHECK_EQ(b[7], 0x00); PB_CHECK_EQ(b[11], 0x01);   // status=0, ndev=1
  // path[256] 从 offset 12 开始
  PB_CHECK_EQ(b[12], '/');  PB_CHECK_EQ(b[13], 'p');
  // busid[32] 从 offset 268；'7' '-' '1'
  PB_CHECK_EQ(b[268], '7'); PB_CHECK_EQ(b[270], '1');
  // busnum u32 BE @272..275 == 7
  PB_CHECK_EQ(b[275], 7);
  // idVendor u32 BE: 0x1234 → b[288]=0x12 b[289]=0x34（busnum,devnum,speed 各4B 后）
}
PB_TEST(usbip_submit_roundtrip) {
  ByteWriter w; // 用 core/bytes
  w.u32be(USBIP_CMD_SUBMIT); w.u32be(0x42); w.u32be(0x00070001); w.u32be(DIR_IN);
  w.u32be(0x81); w.u32be(0); w.u32be(8); w.u32be(0); w.u32be(0); w.u32be(1); w.u32be(10);
  uint8_t setup[8] = {0x81,6,0,1,0,0,2,0}; w.bytes(setup,8);
  SubmitHeader h; PB_CHECK(parse_submit(w.buf().data(), w.buf().size(), h));
  PB_CHECK_EQ(h.seqnum, 0x42u); PB_CHECK_EQ(h.ep, 0x81u); PB_CHECK_EQ(h.setup[0], 0x81);
  PB_CHECK_EQ(h.transfer_buffer_length, 8);
}
PB_TEST(usbip_ret_submit_in_carries_data) {
  uint8_t data[] = {0xAA, 0xBB};
  auto b = pack_ret_submit(7, 1, 0x81, DIR_IN, 0, 2, data, 2);
  PB_CHECK_EQ(b.size(), 42u);
  PB_CHECK_EQ(b[0], 0x00); PB_CHECK_EQ(b[3], 0x03);    // RET_SUBMIT
  PB_CHECK_EQ(b[4], 0); PB_CHECK_EQ(b[7], 7);          // seqnum BE
  PB_CHECK_EQ(b[40], 0xAA); PB_CHECK_EQ(b[41], 0xBB);
}
```
（`using pb::ByteWriter;` 补充头引用。）`tests/CMakeLists.txt` 增加新测试源。
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（纯函数；`pack_rep_import` 与 devlist 的单设备段共用内部打包函数）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe usbip` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(usbip): 线协议编解码"`

### Task 8: USB 描述符构建器

**Files:**
- Create: `src/usb/descriptors.h`, `src/usb/descriptors.cpp`
- Test: `tests/unit/test_descriptors.cpp`

**Interfaces:**
```cpp
namespace pb::usb {
#pragma pack(push,1)   // 亦提供固定布局结构体（小端由 x64 保证）
struct UsbDeviceDescriptor {           // 18B
  uint8_t bLength=18, bDescriptorType=1; uint16_t bcdUSB=0x0200;
  uint8_t bDeviceClass=0, bDeviceSubClass=0, bDeviceProtocol=0, bMaxPacketSize0=8;
  uint16_t idVendor=0, idProduct=0, bcdDevice=0x0100;
  uint8_t iManufacturer=1, iProduct=2, iSerialNumber=3, bNumConfigurations=1;
};
struct EndpointDef { uint8_t address; uint8_t attributes; uint16_t max_packet; uint8_t interval; };
struct HidClassDescriptor { uint16_t bcdHid=0x0111; uint8_t country=0; uint8_t num_descriptors=1;
  uint8_t report_type=0x22; uint16_t report_length; };  // 9B 类描述符
struct InterfaceDef {
  uint8_t number, alt_setting, iclass, isubclass, iprotocol, str_index=0;
  std::optional<HidClassDescriptor> hid; std::vector<EndpointDef> endpoints;
};
struct IadDef { uint8_t first_interface, interface_count, iclass, isubclass, iprotocol; std::string function_str_unused; }; // 8B
struct ConfigParams {
  uint16_t vid, pid; std::string product;      // 字符串索引 2
  uint8_t max_packet0 = 8; bool self_powered=false, remote_wakeup=true;
  std::vector<IadDef> iads; std::vector<InterfaceDef> interfaces;
  uint8_t attributes_override = 0;             // 优先于 self_powered/remote_wakeup 计算
};
std::vector<uint8_t> build_device_descriptor(const ConfigParams& c);          // 18B
std::vector<uint8_t> build_config_descriptor(const ConfigParams& c);          // 9 + Σ(9+可选9(HID)+7×EP + 可选8(IAD))
std::vector<uint8_t> build_string_descriptor_zero();                          // LANGID 0x0409 → 4B
std::vector<uint8_t> build_string_descriptor(std::string_view utf8);          // UTF-8→UTF-16LE
std::vector<uint8_t> build_serial_descriptor(std::string_view serial);
}
```
规则：config wTotalLength/bNumInterfaces 由内容计算；attributes = 0x80 | (self_powered?0x40:0) | (remote_wakeup?0x20:0)；bMaxPower=50(100mA)。HID 类描述符插在 interface 描述符之后、端点之前；IAD 插在其覆盖的首接口之前。

- [ ] **Step 1: 失败测试** `tests/unit/test_descriptors.cpp`（golden 字节断言）：
```cpp
#include "test/test_framework.h"
#include "usb/descriptors.h"
using namespace pb::usb;
PB_TEST(desc_device_minimal) {
  ConfigParams c; c.vid=0x1234; c.pid=0x5678; c.product="phantom gamepad";
  auto d = build_device_descriptor(c);
  PB_CHECK_EQ(d.size(), 18u);
  PB_CHECK_EQ(d[0], 18u); PB_CHECK_EQ(d[1], 1u);
  PB_CHECK_EQ(d[4], 8u);                        // bMaxPacketSize0
  PB_CHECK_EQ(d[8], 0x34); PB_CHECK_EQ(d[9], 0x12);  // idVendor LE
  PB_CHECK_EQ(d[17], 1u);
}
PB_TEST(desc_config_hid_interrupt) {
  ConfigParams c; c.vid=1; c.pid=2; c.product="x";
  InterfaceDef intf{0,0,3,0,0,0, HidClassDescriptor{.report_length=35}, {{0x81,3,8,10}}};
  c.interfaces = {intf};
  auto cfg = build_config_descriptor(c);
  // 9(config)+9(iface)+9(hid)+7(ep) = 34
  PB_CHECK_EQ(cfg.size(), 34u);
  PB_CHECK_EQ(cfg[1], 2u);                       // CONFIG type
  PB_CHECK_EQ(cfg[4], 34u); PB_CHECK_EQ(cfg[5], 0u);   // wTotalLength LE
  PB_CHECK_EQ(cfg[4+1], 4u);                     // bNumInterfaces=1
  PB_CHECK_EQ(cfg[9+1], 4u);                     // interface type=4
  PB_CHECK_EQ(cfg[18+1], 0x21u);                 // HID class desc
  PB_CHECK_EQ(cfg[18+7], 35u);                   // report descriptor len LE
  PB_CHECK_EQ(cfg[27+1], 5u);                    // endpoint type=5
  PB_CHECK_EQ(cfg[27+2], 0x81u); PB_CHECK_EQ(cfg[27+3], 3u);  // INT IN, interrupt
}
PB_TEST(desc_config_bluetooth_two_interfaces) {
  ConfigParams c; c.vid=0x0B00; c.pid=0x0001; c.product="phantom bt";
  InterfaceDef i0{0,0,0xE0,1,1,0,std::nullopt,{{0x81,3,16,1},{0x02,2,64,0},{0x82,2,64,0}}};
  InterfaceDef i1{1,0,0xE0,1,1,0,std::nullopt,{}};
  c.interfaces = {i0, i1};
  auto cfg = build_config_descriptor(c);
  PB_CHECK_EQ(cfg.size(), 9u + 9u+7*3 + 9u);     // 47：config + if0(3EP) + if1(无EP)
  PB_CHECK_EQ(cfg[4], 47u);
  PB_CHECK_EQ(cfg[5], 0u);
  PB_CHECK_EQ(cfg[9], 0xE0u);                    // iface0 class
  PB_CHECK_EQ(cfg[25+2], 0x81u); PB_CHECK_EQ(cfg[32+2], 0x02u); PB_CHECK_EQ(cfg[39+2], 0x82u);
}
PB_TEST(desc_string_utf16le) {
  auto s = build_string_descriptor("AB");
  PB_CHECK_EQ(s.size(), 6u);                     // 2 header + 2×2
  PB_CHECK_EQ(s[0], 6u); PB_CHECK_EQ(s[1], 3u);
  PB_CHECK_EQ(s[2], 'A'); PB_CHECK_EQ(s[4], 'B');
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现** → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe desc` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(usb): 描述符构建"`

### Task 9: USB 设备核心（控制传输分发 + 挂起 IN 端点）

**Files:**
- Create: `src/usb/device.h`, `src/usb/device.cpp`, `src/usb/control.h`, `src/usb/control.cpp`
- Test: `tests/unit/test_control.cpp`

**Interfaces:**
```cpp
namespace pb::usb {
struct ControlSetup { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; };
ControlSetup parse_setup(const uint8_t s[8]);                 // LE 解码
enum class CtrlStage { Setup, DataOut, StatusIn };
// 标准请求号
constexpr uint8_t REQ_GET_STATUS=0, REQ_CLEAR_FEATURE=1, REQ_SET_FEATURE=3, REQ_SET_ADDRESS=5,
                  REQ_GET_DESCRIPTOR=6, REQ_SET_DESCRIPTOR=7, REQ_GET_CONFIGURATION=8,
                  REQ_SET_CONFIGURATION=9, REQ_GET_INTERFACE=10, REQ_SET_INTERFACE=11;
constexpr uint8_t DT_DEVICE=1, DT_CONFIG=2, DT_STRING=3, DT_INTERFACE=4, DT_ENDPOINT=5,
                  DT_DEVICE_QUALIFIER=6, DT_IAD=0x0B, DT_HID_REPORT=0x22, DT_BOS=0x0F;

// 设备模型接口：HID/BT 模型实现之
struct IUsbModelSink {  // 由 server 提供给模型，用于异步完成 IN 传输
  virtual ~IUsbModelSink() = default;
  virtual void complete_in(uint8_t ep_addr, int32_t status, std::vector<uint8_t> data) = 0;
  virtual void stall_ep(uint8_t ep_addr) = 0;      // 对挂起传输回 EPIPE(32)
};
class IUsbDeviceModel {
 public:
  virtual ~IUsbDeviceModel() = default;
  virtual const usbip::ExportedDevice& exported() const = 0;   // devlist 元数据
  // EP0 控制传输：data 为 OUT 数据阶段内容（wLength>0 且方向 OUT 时非空）
  // 返回：应答回传数据（IN 方向）；抛 ControlStall 表示 STALL
  virtual std::vector<uint8_t> on_control(const ControlSetup& s, const std::vector<uint8_t>& data) = 0;
  // 数据 OUT 传输到达（bulk/interrupt OUT）
  virtual void on_out(uint8_t ep_addr, std::vector<uint8_t> data) = 0;
  // IN 传输到达（Windows 轮询）：立即有数据→返回 true 并经 sink 回填；否则记录关注、稍后 complete_in
  virtual void on_in(uint8_t ep_addr) = 0;
  virtual void on_stall_clear(uint8_t ep_addr) {}  // CLEAR_FEATURE(ENDPOINT_HALT)
  virtual void on_detach() = 0;
};
}
```
`control.cpp` 提供：`bool descriptor_request(const ControlSetup&, const DescriptorStore&, std::vector<uint8_t>& out)` 处理 GET_DESCRIPTOR（device/config/string/index 化）与 SET_ADDRESS/SET_CONFIGURATION/GET_CONFIGURATION/GET_INTERFACE/SET_INTERFACE/GET_STATUS 的标准应答骨架，未实现项返回 false 交由模型处置。
`DescriptorStore{ device, config, strings(map idx→bytes), report_descriptor }` 由模型填充。

- [ ] **Step 1: 失败测试** `tests/unit/test_control.cpp`：
```cpp
#include "test/test_framework.h"
#include "usb/control.h"
using namespace pb::usb;
static DescriptorStore store() {
  DescriptorStore s; ConfigParams c; c.vid=1;c.pid=2;c.product="t";
  InterfaceDef i{0,0,3,0,0,0,std::nullopt,{{0x81,3,8,1}}}; c.interfaces={i};
  s.device = build_device_descriptor(c); s.config = build_config_descriptor(c);
  s.strings[0] = build_string_descriptor("t"); s.report_descriptor = {0xC0};
  return s;
}
PB_TEST(control_get_descriptor_device) {
  auto s = store(); std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0100, 0, 18));
  PB_CHECK(descriptor_request(su, s, out));
  PB_CHECK_EQ(out.size(), 18u); PB_CHECK_EQ(out[0], 18u);
}
PB_TEST(control_get_descriptor_config_truncated_by_wlength) {
  auto s = store(); std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0200, 0, 4));
  PB_CHECK(descriptor_request(su, s, out));
  PB_CHECK_EQ(out.size(), 4u);                    // 截断到 wLength
}
PB_TEST(control_unknown_falls_back) {
  auto s = store(); std::vector<uint8_t> out;
  auto su = parse_setup(make_setup8(0x80, REQ_GET_DESCRIPTOR, 0x0E00, 0, 8)); // DT 格式错误
  PB_CHECK(!descriptor_request(su, s, out));
}
```
辅助 `make_setup8(bm,br,wv,wi,wl)` 返回 8 字节数组（本任务在测试内定义）。
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现** → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe control` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(usb): 控制传输分发"`

### Task 10: HID 报告描述符模板与 Profile

**Files:**
- Create: `src/hid/report_descriptor.h`, `src/hid/report_descriptor.cpp`, `src/hid/profiles.h`, `src/hid/profiles.cpp`, `profiles/gamepad.json`, `profiles/keyboard.json`, `profiles/mouse.json`, `profiles/vendor.json`
- Test: `tests/unit/test_report_descriptor.cpp`, `tests/unit/test_profiles.cpp`

**Interfaces:**
```cpp
namespace pb::hid {
// 模板：每个都包含一个 vendor-defined 顶层集合（R-107 缓解：保证 WinRT HidDevice 可打开）
std::vector<uint8_t> template_gamepad();   // report id 1: 16 buttons + hat + X/Y/Z/RZ；另附 vendor 集合
std::vector<uint8_t> template_keyboard();  // boot 键盘 8 字节 + vendor 集合
std::vector<uint8_t> template_mouse();     // boot 鼠标 3 键 + X/Y + 滚轮 + vendor 集合
std::vector<uint8_t> template_vendor();    // 纯 vendor：input64 + output64
std::vector<uint8_t> template_ble_keyboard_reportmap(); // BLE HOGP 用（无 vendor 集合，Task 22 复用）

struct HidProfile {                     // 从 JSON 加载
  std::string template_name;            // gamepad|keyboard|mouse|vendor
  uint16_t vid, pid; std::string product, manufacturer = "Phantom-BT", serial;
  std::optional<std::vector<uint8_t>> report_descriptor_override;  // hex 覆盖
  static HidProfile from_json(const pb::json::Value& v);  // 抛错带字段名
  static HidProfile load_file(const std::string& path);
};
}
```
`profiles/*.json` 示例（gamepad.json）：
```json
{ "template": "gamepad", "vid": "0x1209", "pid": "0xB007", "product": "Phantom Gamepad", "serial": "PG-0001" }
```
vendor.json: `{"template":"vendor","vid":"0x1209","pid":"0xB00D","product":"Phantom Raw HID"}`；keyboard.json `pid 0xB001`；mouse.json `pid 0xB002`。
JSON 中 vid/pid 接受 "0x####" 或十进制字符串；实现统一 `parse_u16(std::string)`。

模板字节必须精确（测试即文档）。gamepad 模板实现（`report_descriptor.cpp` 直接写常量数组并附注释）：
```
0x05,0x01, 0x09,0x05, 0xA1,0x01,            // UsagePage(GD) Usage(Gamepad) Coll(App)
 0x15,0x00, 0x26,0xFF,0x00,                  // Logical 0..255
 0x35,0x00, 0x46,0xFF,0x00, 0x65,0x14,       // Physical 0..255, Units(ENG_ROT=20)? 简化: 省略 units
 0x75,0x04, 0x95,0x01, 0x25,0x07, 0x46,0x3B,0x01, 0x66,0x14,0x00, 0x81,0x42, // Hat switch(4bit, null=7)
 0x75,0x01, 0x95,0x10, 0x25,0x01, 0x45,0x01, 0x81,0x02,                      // Buttons 1..16
 0x75,0x08, 0x95,0x04, 0x26,0xFF,0x00, 0x46,0xFF,0x00, 0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35,
 0x81,0x02,                                                                  // X Y Z RZ
 0x05,0x01, 0x09,0x39, ...                                                   // （见下：统一用简化定义）
```
**简化决定（写死为可实现常量）**：gamepad = `usagePage GD, usage Gamepad, Coll App; [hat 4b null-state] [buttons 16×1b] [X,Y,Z,RZ 各 8b] EndColl` + `vendor 集合（同 template_vendor 字节）`，report id 不使用（单报告）。测试断言：首 6 字节 `05 01 09 05 A1 01`；总长度 = 常量数；`0xC0` 收尾数量≥2。keyboard/mouse 同理给常量数组（keyboard: `05 01 09 06 A1 01 85 01 95 08 75 01 15 00 25 01 05 07 19 E0 29 E7 81 00 81 01 95 01 75 08 81 01 95 06 75 08 15 00 26 FF 00 05 07 19 00 2A FF 00 81 00 C0` + vendor 集合；mouse: `05 01 09 02 A1 01 09 01 A1 00 85 01 05 09 19 01 29 03 15 00 25 01 95 03 75 01 81 02 95 01 75 05 81 03 05 01 09 30 09 31 16 01 80 26 FF 7F 75 10 95 02 81 06 09 38 15 81 25 7F 75 08 95 01 81 06 C0 C0` + vendor 集合）。vendor 集合常量：
```
06 00 FF 09 01 A1 01 09 02 15 00 26 FF 00 75 08 95 40 91 02 09 03 95 40 81 02 C0
```
（长度 24 字节：逐字节计数在测试断言中验证。）

- [ ] **Step 1: 失败测试** `tests/unit/test_report_descriptor.cpp`：
```cpp
#include "test/test_framework.h"
#include "hid/report_descriptor.h"
using namespace pb::hid;
PB_TEST(rdesc_gamepad_shape) {
  auto d = template_gamepad();
  PB_CHECK_EQ(d[0], 0x05); PB_CHECK_EQ(d[1], 0x01); PB_CHECK_EQ(d[3], 0x05); PB_CHECK_EQ(d[4], 0xA1);
  PB_CHECK_EQ(d.back(), 0xC0);
  PB_CHECK(std::count(d.begin(), d.end(), 0xC0) >= 2);      // 主集合+vendor集合
}
PB_TEST(rdesc_vendor_template_exact) {
  auto d = template_vendor();
  static const uint8_t expect[] = {0x06,0x00,0xFF,0x09,0x01,0xA1,0x01,0x09,0x02,0x15,0x00,
    0x26,0xFF,0x00,0x75,0x08,0x95,0x40,0x91,0x02,0x09,0x03,0x95,0x40,0x81,0x02,0xC0};
  PB_CHECK_EQ(d.size(), sizeof(expect));
  PB_CHECK(std::equal(d.begin(), d.end(), expect));
}
PB_TEST(rdesc_keyboard_has_vendor_collection) {
  auto d = template_keyboard();
  auto d2 = template_vendor();
  PB_CHECK(std::search(d.begin(), d.end(), d2.begin(), d2.end()) != d.end());
}
```
`tests/unit/test_profiles.cpp`：
```cpp
PB_TEST(profile_load_gamepad_json) {
  auto p = pb::hid::HidProfile::load_file("profiles/gamepad.json");
  PB_CHECK_EQ(p.template_name, std::string("gamepad"));
  PB_CHECK_EQ(p.vid, 0x1209u); PB_CHECK_EQ(p.pid, 0xB007u);
  PB_CHECK_EQ(p.product, std::string("Phantom Gamepad"));
}
PB_TEST(profile_rejects_unknown_template) {
  auto v = pb::json::parse("{\"template\":\"printer\",\"vid\":\"0x1\",\"pid\":\"0x2\",\"product\":\"x\"}");
  PB_CHECK_THROWS(pb::hid::HidProfile::from_json(v));
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（模板常量 + `parse_u16` + from_json/load_file 用 `std::ifstream`）→ **Step 4: 运行** `build\debug\tests\phantombt-tests.exe rdesc` 与 `profile` — passed（测试运行目录为仓库根：`tests/CMakeLists.txt` 设 `VS_DEBUGGER_WORKING_DIRECTORY` 或 main 里 `SetCurrentDirectory` 到 `${CMAKE_SOURCE_DIR}`——采用后者：main 开头 `SetCurrentDirectoryA(PB_SOURCE_DIR)`，宏由 `target_compile_definitions(... PB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")` 注入）
- [ ] **Step 5: Commit** `git commit -am "feat(hid): 报告描述符模板与JSON Profile"`

### Task 11: HID 设备模型

**Files:**
- Create: `src/hid/hid_model.h`, `src/hid/hid_model.cpp`
- Test: `tests/unit/test_hid_model.cpp`

**Interfaces:**
```cpp
namespace pb::hid {
class HidModel : public pb::usb::IUsbDeviceModel {
 public:
  HidModel(const HidProfile& p, uint32_t busnum, uint32_t devnum);  // 内部构建 DescriptorStore
  // 控制：GET_DESCRIPTOR(rep→report_descriptor)、GET_REPORT/SET_REPORT(类请求 01/09)、
  //       GET_IDLE/SET_IDLE(02/0A)、GET_PROTOCOL/SET_PROTOCOL(03/0B)
  std::vector<uint8_t> on_control(const pb::usb::ControlSetup&, const std::vector<uint8_t>&) override;
  void on_out(uint8_t ep, std::vector<uint8_t> data) override;  // INT OUT → 输出报告存队列
  void on_in(uint8_t ep) override;                              // INT IN：有报告→立即 complete_in
  void on_detach() override;
  // 测试/控制 API
  void inject_input_report(const std::vector<uint8_t>& report);  // 唤醒挂起 INT IN 或入队
  std::optional<std::vector<uint8_t>> pop_output_report();
  void set_idle(uint8_t duration_4ms); uint8_t idle() const;
  bool stalled_recently = false;                                // 测试观测 STALL
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
```
行为：挂起 INT IN 最多 1 个（Windows 轮询语义）；inject 时无挂起则排队（容量 16，满则丢最旧+计数）；`on_control` 未识别类请求 → STALL（抛 `pb::usb::ControlStall`）并置 stalled_recently。sink 由 server 注入（构造后 `attach_sink(IUsbModelSink*)`——加入接口：`void attach_sink(pb::usb::IUsbModelSink* s)`）。

- [ ] **Step 1: 失败测试** `tests/unit/test_hid_model.cpp`：
```cpp
#include "test/test_framework.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"
struct FakeSink final : pb::usb::IUsbModelSink {
  int completes = 0, stalls = 0; std::vector<std::vector<uint8_t>> data;
  void complete_in(uint8_t, int32_t st, std::vector<uint8_t> d) override { ++completes; data.push_back(std::move(d)); }
  void stall_ep(uint8_t) override { ++stalls; }
};
PB_TEST(hid_get_report_descriptor_via_control) {
  auto prof = pb::hid::HidProfile::from_json(pb::json::parse(
    "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\",\"product\":\"v\"}"));
  pb::hid::HidModel m(prof, 7, 1);
  auto& exp = m.exported(); PB_CHECK_EQ(exp.vid, 0x1209u);
  uint8_t setup[8] = {0x81, 0x06, 0x00, 0x22, 0x00, 0x00, 0xFF, 0x00};
  auto resp = m.on_control(pb::usb::parse_setup(setup), {});
  PB_CHECK_EQ(resp.size(), 26u);                       // template_vendor 长度
}
PB_TEST(hid_inject_completes_pending) {
  FakeSink sink; auto m = make_vendor_model(&sink);    // 测试内 helper：构造+attach_sink
  m->on_in(0x81);                                       // Windows 轮询先到
  PB_CHECK_EQ(sink.completes, 0);
  m->inject_input_report({0x01, 0x02, 0x03});
  PB_CHECK_EQ(sink.completes, 1);
  PB_CHECK_EQ(sink.data[0].size(), 3u);
  m->inject_input_report({0x04});                       // 无挂起→入队
  m->on_in(0x81);
  PB_CHECK_EQ(sink.completes, 2);
}
PB_TEST(hid_unknown_class_request_stalls) {
  FakeSink sink; auto m = make_vendor_model(&sink);
  uint8_t setup[8] = {0xA1, 0x7F, 0,0, 0,0, 0,0};       // 保留类请求
  bool threw = false; try { m->on_control(pb::usb::parse_setup(setup), {}); } catch (pb::usb::ControlStall&) { threw = true; }
  PB_CHECK(threw); PB_CHECK(m->stalled_recently);
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现** → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe hid` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(hid): USB HID 设备模型"`

### Task 12: usbip 服务器（devlist/import/URB 循环）

**Files:**
- Create: `src/usbip/server.h`, `src/usbip/server.cpp`, `src/sim/device_registry.h`, `src/sim/device_registry.cpp`
- Test: `tests/unit/test_usbip_server.cpp`（回环 TCP，fake vhci 客户端）

**Interfaces:**
```cpp
namespace pb::usbip {
class Server {
 public:
  Server(pb::sim::DeviceRegistry& reg, uint16_t port);   // port 0=自动
  bool start(std::string* err);                          // 监听线程（每连接一个线程）
  void stop();                                           // 关闭并 join；连接对象线程安全退出
  uint16_t port() const;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
// src/sim/device_registry.h
namespace pb::sim {
class DeviceRegistry {
 public:
  std::string add(std::shared_ptr<pb::usb::IUsbDeviceModel> dev);  // 返回 busid "7-N"，N 自增
  bool remove(const std::string& busid);
  std::shared_ptr<pb::usb::IUsbDeviceModel> find(const std::string& busid) const;
  std::vector<pb::usbip::ExportedDevice> list() const;
  std::mutex& mutex();                                   // server 与控制命令共用
 private: std::vector<std::pair<std::string, std::shared_ptr<pb::usb::IUsbDeviceModel>>> devices_; uint32_t next_ = 1;
};
}
```
**服务器行为规范：**
1. 接受连接 → 收 4B opcode：`OP_REQ_DEVLIST` → 回 `pack_rep_devlist(reg.list())`；`OP_REQ_IMPORT`（随后 20B busid 以 `\0` 结尾字符串，msg 格式 = 4B opcode + 200B? —— 按内核：busid 为 `char[SYSFS_BUS_ID_SIZE=32]` 紧跟 opcode）→ 查 registry：命中回 `pack_rep_import`（**每次 import 独占该设备**——同一 busid 二次 import 回 status=1），并把该连接绑定到设备模型（注入 sink）；未命中回 `OP_REP_IMPORT, status=1`。
2. 绑定后循环：48B 头解出 `CMD_SUBMIT`：`ep==0` → 控制传输。收 data stage（若 direction==OUT 且 `transfer_buffer_length>0`，先收该长度字节）→ 调 `model->on_control/setup 解析`：IN → `pack_ret_submit(..., actual=resp.size(), resp)`；OUT(无数据阶段) → 执行后回 `actual=0`；`ControlStall` → `ret_submit(status=EPIPE=32)`。非 ep0：direction==OUT → 收 data → `model->on_out(ep, data)` → 回 `actual=data.size()`；direction==IN → 调 `model->on_in(ep)`，服务器为该 (ep) 挂起 URB，等 `sink.complete_in(ep, st, data)` → 回 `ret_submit(status, actual=data.size(), data)`。
3. `CMD_UNLINK`：找 seqnum 匹配的挂起 IN：在→丢弃回 `ret_unlink(0)`；不在→回 `ret_unlink(-1)`（内核语义：已完成的 unlink 返回 0 或 -1 均可容忍）。
4. 连接断开 → `model->on_detach()` + 取消挂起。
5. 全部 URB 收发/完成写 PB_LOG_DEBUG（hex 截断 64B）。

- [ ] **Step 1: 失败测试** `tests/unit/test_usbip_server.cpp`：
```cpp
#include "test/test_framework.h"
#include "usbip/server.h"
#include "sim/device_registry.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"
using namespace pb;
static std::shared_ptr<hid::HidModel> add_vendor_device(sim::DeviceRegistry& reg) {
  auto prof = hid::HidProfile::from_json(json::parse(
    "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\",\"product\":\"v\"}"));
  auto m = std::make_shared<hid::HidModel>(prof, 7, 1);
  reg.add(m); return m;
}
PB_TEST(server_devlist_import_control_interrupt) {
  net::WsaSession w; sim::DeviceRegistry reg; auto model = add_vendor_device(reg);
  usbip::Server srv(reg, 0); std::string err; PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err); PB_CHECK(c.is_open());
  // devlist
  ByteWriter w1; w1.u32be(usbip::OP_REQ_DEVLIST); c.send_all(w1.buf().data(), w1.buf().size(), &err);
  uint8_t h[8]; c.recv_exact(h, 8, &err);
  PB_CHECK_EQ((uint32_t)(h[3]), 1u);                    // OP_REP_DEVLIST
  // …读完整包（用 recv_exact 读 12+312+4-8）并跳过内容…
  std::vector<uint8_t> rest(312 + 4 + 4 - 8 + 0);       // ndev(4B 已含于头12，此处只补足到包尾)
  // （实现细节：先读 4B status+4B ndev，再按 ndev 读 312+4*n —— 测试中直接按此读）
  // import
  ByteWriter w2; w2.u32be(usbip::OP_REQ_IMPORT);
  char busid[32] = "7-1"; w2.bytes((uint8_t*)busid, 32);
  c.send_all(w2.buf().data(), w2.buf().size(), &err);
  uint8_t imp[8]; c.recv_exact(imp, 8, &err);
  PB_CHECK_EQ((uint32_t)(imp[3]), 4u);                  // OP_REP_IMPORT
  std::vector<uint8_t> devblk(312); c.recv_exact(devblk.data(), 312, &err);
  // 控制传输 GET_DESCRIPTOR device
  ByteWriter w3;
  w3.u32be(usbip::USBIP_CMD_SUBMIT); w3.u32be(1); w3.u32be(0x00070001); w3.u32be(usbip::DIR_IN);
  w3.u32be(0); w3.u32be(0); w3.u32be(18); w3.u32be(0); w3.u32be(0); w3.u32be(0); w3.u32be(0);
  uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0}; w3.bytes(setup, 8);
  c.send_all(w3.buf().data(), w3.buf().size(), &err);
  std::vector<uint8_t> ret(40 + 18); c.recv_exact(ret.data(), ret.size(), &err);
  PB_CHECK_EQ(ret[3], 3u);                              // RET_SUBMIT
  PB_CHECK_EQ(ret[40], 18u); PB_CHECK_EQ(ret[41], 1u);  // device descriptor 起点
  // 中断 IN 轮询 → 挂起 → 注入完成
  ByteWriter w4;
  w4.u32be(usbip::USBIP_CMD_SUBMIT); w4.u32be(2); w4.u32be(0x00070001); w4.u32be(usbip::DIR_IN);
  w4.u32be(0x81); w4.u32be(0); w4.u32be(64); w4.u32be(0); w4.u32be(0); w4.u32be(0); w4.u32be(10);
  uint8_t zero8[8] = {}; w4.bytes(zero8, 8);
  c.send_all(w4.buf().data(), w4.buf().size(), &err);
  Sleep(150);                                            // 服务器挂起 URB
  model->inject_input_report({9, 9});
  std::vector<uint8_t> ret2(40 + 2); c.recv_exact(ret2.data(), ret2.size(), &err);
  PB_CHECK_EQ(ret2[3], 3u); PB_CHECK_EQ(ret2[40], 9u);
  // unlink 挂起 URB
  // …（seqnum=3 再发一个 IN 轮询，然后 CMD_UNLINK seqnum=3 → ret_unlink status=0）
  c.close(); srv.stop();
}
```
（测试中"…"处照注释补全代码，不留占位——执行时写成完整可编译代码。）
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（每连接线程 + `IUsbModelSink` 适配器把 `complete_in` 投递回连接写队列——互斥保护 socket 写；挂起表 `map<ep_addr, PendingIn{seqnum, unlink_requested}>`）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe server` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(usbip): TCP 服务器与 URB 循环"`

### Task 13: Simulator 编排 + 控制命令 + CLI + M0 集成

**Files:**
- Create: `src/sim/simulator.h`, `src/sim/simulator.cpp`, `src/control/commands.h`, `src/control/commands.cpp`, `src/cli/main.cpp`, `tools/run-m0.ps1`, `tools/find-usbip.ps1`
- Test: `tests/unit/test_commands.cpp`（命令层直测）

**Interfaces:**
```cpp
namespace pb::sim {
class Simulator {
 public:
  Simulator();                                // 内建 DeviceRegistry + usbip::Server(3240，可重试递增)
  bool start(std::string* err); void stop();
  // 命令实现（返回给 CLI 的文本行；失败抛 std::runtime_error）
  std::string cmd_load_hid_profile(const std::string& json_path);
  std::string cmd_list();
  std::string cmd_attach(const std::string& busid);   // 调 usbip.exe（见下）
  std::string cmd_detach(const std::string& busid);
  std::string cmd_inject(const std::string& busid, const std::vector<uint8_t>& report);
  uint16_t port() const;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
namespace pb::control {
// 命令语法（一行一条；空格分隔；# 注释）：
//  load-hid <path.json>        → OK <busid>
//  list                        → OK <busid> <vid:pid> <product> 每设备一行，最后 OK-END
//  attach <busid> | attach-all → OK
//  detach <busid>              → OK
//  inject <busid> <hexbytes>   → OK
//  status                      → OK usbip-port=<p> devices=<n>
std::vector<std::string> execute(Simulator& sim, const std::string& line); // 逐条执行；异常→ {"ERR <msg>"}
std::string usage();
}
```
`cmd_attach` 实现：`tools/find-usbip.ps1` 逻辑内联为 C++：依次探测 `%ProgramFiles%\\USBip\\usbip.exe`、`where usbip.exe`；找到后 `_popen`：`usbip.exe attach -r 127.0.0.1 -p <port> -b <busid>`（usbip-win2 语法；失败再试旧语法 `attach -r 127.0.0.1 -b <busid>`），捕获输出与退出码写日志。找不到 usbip.exe → 抛错提示安装路径。

`src/cli/main.cpp`：`phantombt serve [--port N]`（前台跑 Simulator，Ctrl-C 退出）；其余子命令（load-hid/list/attach/detach/inject/status）直连本进程内 Simulator？——**决定：CLI 子命令也内嵌 Simulator（无守护进程时单命令模式无意义）**，即 CLI 全部子命令要求"serve 已在另一进程"的场景用命名管道（Task 25）。本任务 CLI 仅实现 `serve`（含内嵌）+ `selftest`（跑一次 devlist→import→GET_DESCRIPTOR 回环冒烟）。

- [ ] **Step 1: 失败测试** `tests/unit/test_commands.cpp`：
```cpp
PB_TEST(commands_load_list_inject) {
  pb::sim::Simulator sim; std::string err; PB_CHECK(sim.start(&err));
  auto r1 = pb::control::execute(sim, "load-hid profiles/vendor.json");
  PB_CHECK_EQ(r1[0].substr(0, 3), std::string("OK "));
  auto busid = r1[0].substr(4);
  auto r2 = pb::control::execute(sim, "list");
  PB_CHECK_EQ(r2.back(), std::string("OK-END"));
  PB_CHECK(std::any_of(r2.begin(), r2.end(), [&](auto& s){ return s.find(busid) != std::string::npos; }));
  auto r3 = pb::control::execute(sim, "inject " + busid + " DEADBEEF");
  PB_CHECK_EQ(r3[0], std::string("OK"));
  auto r4 = pb::control::execute(sim, "inject 9-9 00");
  PB_CHECK_EQ(r4[0].substr(0, 3), std::string("ERR"));
  auto r5 = pb::control::execute(sim, "bogus-cmd");
  PB_CHECK_EQ(r5[0].substr(0, 3), std::string("ERR"));
  sim.stop();
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（commands.cpp 解析 + Simulator 聚合；cli/main.cpp 用 `std::getline` 参数解析；CMake 加 `add_executable(phantombt src/cli/main.cpp)` 链接 phantom_core）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe commands` — passed；手工 `build\debug\cli\phantombt.exe selftest` 输出 OK
- [ ] **Step 5: Commit** `git commit -am "feat(sim,control,cli): 编排器、命令层与CLI"`

**M0 集成脚本** `tools/run-m0.ps1`：
```powershell
param([switch]$SkipRealDevice)
# 1) 构建 2) 启动 phantombt serve（后台 Job）3) load-hid profiles/gamepad.json
# 4) 若 -SkipRealDevice 或无管理员/无 usbip.exe：执行回环断言（TCP devlist/import/GET_DESCRIPTOR/中断IN注入，
#    复用 tests/unit/test_usbip_server.cpp 的逻辑以独立小工具 build/tools/usbip-loopback.exe 重放）→ 输出 PASS(loopback)
# 5) 否则：attach → 跑 winrt-probe hid --vid 0x1209 --pid 0xB007 --usage-page 0xFF00 --usage 0x01 → detach
# 6) 汇总 JSON 写 integration-results/m0.json；退出码 0=PASS 1=FAIL 3=SKIP(真机段)
```
（winrt-probe 在 Task 13 同步给出最小 hid 模式骨架 `src/probes/main.cpp`：解析参数 → `HidDevice::GetDeviceSelector(usagePage,usage,vid,pid)` → `DeviceInformation::FindAllAsync` → `FromIdAsync` → 订阅 `InputReportReceived` → 等待报告或超时 → 输出 JSON 行 + 退出码。C++/WinRT 头：`winrt/Windows.Devices.HumanInterfaceDevice.h`、`winrt/Windows.Devices.Enumeration.h`、`winrt/Windows.Foundation.h`；CMake：`target_link_libraries(winrt-probe PRIVATE windowsapp)` + `target_compile_definitions` `_WIN32_WINNT=0x0A00`；构建前检查 `%WindowsSdkDir%` 的 cppwinrt 头存在，缺则该目标跳过并输出警告（CMake `check_include_file_path` 或 `if(EXISTS ...)`）。
probe 用法：`winrt-probe.exe hid --vid 0x1209 --pid 0xB007 --usage-page 0xFF00 --usage 0x01 --expect-hex 0102 --wait-ms 5000`；输出 `{"result":"pass","reports":["0102"]}` 或 `{"result":"fail","reason":"open"|"noreport"|"timeout"}`；退出码 0/2/3。）

- [ ] **Step 6: 跑 M0 脚本** Run: `powershell -File tools/run-m0.ps1 -SkipRealDevice` — Expected: PASS(loopback)
- [ ] **Step 7: Commit** `git commit -am "feat(integration): M0 回环与真机脚本 + winrt-probe hid"`

---

## M1 虚拟蓝牙适配器

### Task 14: HCI 报文编解码

**Files:**
- Create: `src/hci/packets.h`, `src/hci/packets.cpp`
- Test: `tests/unit/test_hci_packets.cpp`

**Interfaces:**
```cpp
namespace pb::hci {
constexpr uint16_t opcode(uint16_t ogf, uint16_t ocf) { return (ogf << 10) | ocf; }
constexpr uint16_t OGF_LINK_CTRL=0x01, OGF_LINK_POLICY=0x02, OGF_HC_BASEBAND=0x03, OGF_INFO=0x04,
                   OGF_STATUS=0x05, OGF_TESTING=0x06, OGF_LE=0x08, OGF_VENDOR=0x3F;
// 常用命令（实现于 controller 命令表）：
//  Reset=opcode(3,0x0003) Read_BD_ADDR=(4,0x0009) Read_Local_Name=(3,0x0014)
//  Read_Buffer_Size=(4,0x0005) Read_Local_Version=(4,0x0001) Read_Local_Supported_Commands=(4,0x0002)
//  Read_Local_Supported_Features=(4,0x0003) Host_Buffer_Size=(3,0x0033) Host_Num_Completed_Packets=(3,0x0035|0x0013)
//  Write_Scan_Enable=(3,0x001A) Write_Inquiry_Mode=(3,0x0045) Write_Page_Timeout=(3,0x0018)
//  Write_Class_Of_Device=(3,0x0024) Write_Authentication_Enable=(3,0x0020)
//  Write_Secure_Connections_Host_Support=(3,0x007A) Write_Simple_Pairing_Mode=(3,0x0056)
//  Set_Event_Mask=(3,0x0001) LE: Set_Event_Mask=(8,0x0001) LE_Read_Buffer_Size=(8,0x0002)
//  LE_Read_Local_Supported_Features=(8,0x0003) LE_Set_Random_Address=(8,0x0005)
//  LE_Set_Advertising_Parameters=(8,0x0006) …=(8,0x0008 data) (8,0x000A enable)
//  LE_Set_Scan_Parameters=(8,0x000B) LE_Set_Scan_Enable=(8,0x000C)
//  LE_Create_Connection=(8,0x000D) LE_Create_Connection_Cancel=(8,0x000E)
//  LE_Read_White_List_Size=(8,0x000F) LE_Clear_White_List=(8,0x0010)
//  LE_Read_Channel_Map=(8,0x0015) LE_Read_Remote_Features=(8,0x0016)
//  LE_Encrypt=(8,0x0017) LE_Rand=(8,0x0018) LE_Start_Encryption=(8,0x0019)
//  LE_Long_Term_Key_Request_Reply=(8,0x001A) LE_Set_Host_Channel_Classification=(8,0x0014)
constexpr uint8_t EVT_INQUIRY_COMPLETE=0x01, EVT_INQUIRY_RESULT=0x02, EVT_COMMAND_COMPLETE=0x0E,
  EVT_COMMAND_STATUS=0x0F, EVT_HARDWARE_ERROR=0x10, EVT_FLUSH_OCCURRED=0x11,
  EVT_ROLE_CHANGE=0x12, EVT_NUMBER_COMPLETED_PKTS=0x13, EVT_MODE_CHANGE=0x14,
  EVT_RETURN_LINK_KEYS=0x15, EVT_PIN_CODE_REQUEST=0x16, EVT_LINK_KEY_REQUEST=0x17,
  EVT_LINK_KEY_NOTIFICATION=0x18, EVT_DATA_BUFFER_OVERFLOW=0x1A, EVT_MAX_SLOTS_CHANGE=0x1B,
  EVT_LOOPBACK_COMMAND=0x19, EVT_DISCONNECTION_COMPLETE=0x05, EVT_ENCRYPTION_CHANGE=0x08,
  EVT_REMOTE_NAME_REQUEST_COMPLETE=0x07, EVT_ENCRYPTION_KEY_REFRESH_COMPLETE=0x30,
  EVT_LE_META=0x3E;
constexpr uint8_t LE_EVT_CONNECTION_COMPLETE=0x01, LE_EVT_ADVERTISING_REPORT=0x02,
  LE_EVT_CONNECTION_UPDATE_COMPLETE=0x03, LE_EVT_READ_REMOTE_FEATURES_COMPLETE=0x04,
  LE_EVT_REMOTE_CONNECTION_PARAMETER_REQUEST=0x06, LE_EVT_DATA_LENGTH_CHANGE=0x07,
  LE_EVT_CONNECTION_ESTABLISHED=0x0A?  //（4.2+; 不用）
enum class Status : uint8_t { Success=0x00, UnknownCmd=0x01, UnknownConnId=0x02, HwFailure=0x03,
  PageTimeout=0x04, AuthFailure=0x05, PinOrKeyMissing=0x06, MemoryCapacity=0x07, ConnTimeout=0x08,
  ConnLimitExceeded=0x09, SyncConnLimit=0x0A,_acl_disallowed=0x0B, CmdDisallowed=0x0C,
  LimitedResources=0x0D, SecurityError=0x11, PersonalResource=0x12, InvalidParams=0x12/*注意区分*/,
  Unspecified=0x1F, UnsupportedFeature=0x11?  // —— 以实现文件中枚举为准（测试仅用 Success/UnknownCmd/CmdDisallowed）
};
struct HciCommand { uint16_t op; std::vector<uint8_t> params; };
inline bool parse_command(const uint8_t* p, size_t n, HciCommand& out) {  // [op LE][len][params]
  if (n < 3 || p[2] != n - 3) return false;
  out.op = p[0] | (p[1] << 8); out.params.assign(p + 3, p + n); return true;
}
std::vector<uint8_t> build_command_complete(uint8_t num_pkts, uint16_t op, const std::vector<uint8_t>& ret_params);
std::vector<uint8_t> build_command_status(uint8_t status, uint8_t num_pkts, uint16_t op);
std::vector<uint8_t> build_le_meta(uint8_t subevent, const std::vector<uint8_t>& params);
// ACL: [handle+flags LE][len LE][data]
constexpr uint16_t ACL_PB_START=0x0000, ACL_PB_CONT=0x1000, ACL_PB_START_AUTO=0x2000;
constexpr uint16_t acl_header(uint16_t handle, uint16_t pb) { return handle | pb; }
struct AclFrame { uint16_t handle; uint16_t pb; std::vector<uint8_t> data; };
inline bool parse_acl(const uint8_t* p, size_t n, AclFrame& out) { /* 4B头+data，校验长度 */ }
inline std::vector<uint8_t> build_acl(uint16_t handle, uint16_t pb, const std::vector<uint8_t>& data);
}
```

- [ ] **Step 1: 失败测试** `tests/unit/test_hci_packets.cpp`：
```cpp
PB_TEST(hci_parse_command) {
  uint8_t p[] = {0x03,0x0C,0x00};                 // Reset
  pb::hci::HciCommand c; PB_CHECK(pb::hci::parse_command(p, 3, c));
  PB_CHECK_EQ(c.op, pb::hci::opcode(3, 0x0003));
  uint8_t bad[] = {0x03,0x0C,0x01,0xAA};          // len 与实际不符
  PB_CHECK(!pb::hci::parse_command(bad, 4, c));
}
PB_TEST(hci_command_complete_layout) {
  auto e = pb::hci::build_command_complete(1, 0x0C03, {0x00});
  static const uint8_t expect[] = {0x0E,0x04,0x01,0x03,0x0C,0x00};
  PB_CHECK_EQ(e.size(), sizeof(expect));
  PB_CHECK(std::equal(e.begin(), e.end(), expect));
}
PB_TEST(hci_acl_roundtrip) {
  auto a = pb::hci::build_acl(0x0005, pb::hci::ACL_PB_START, {1,2,3});
  PB_CHECK_EQ(a.size(), 7u);
  pb::hci::AclFrame f; PB_CHECK(pb::hci::parse_acl(a.data(), a.size(), f));
  PB_CHECK_EQ(f.handle, 5u); PB_CHECK_EQ(f.data.size(), 3u);
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（注释中的命令/事件常量全部落为 constexpr）→ **Step 4: 运行** `build\debug\tests\phantombt-tests.exe hci_` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(hci): 报文编解码"`

### Task 15: MiniController——初始化子集

**Files:**
- Create: `src/hci/controller.h`, `src/hci/controller.cpp`
- Test: `tests/unit/test_controller.cpp`

**Interfaces:**
```cpp
namespace pb::hci {
struct ControllerConfig {
  std::array<uint8_t,6> bd_addr = {0x11,0x22,0x33,0x44,0x55,0x66};  // 小端存 LSB first
  std::string local_name = "Phantom-BT Radio";
  uint16_t acl_mtu = 1021; uint8_t sco_mtu = 0; uint16_t sco_packets = 0;   // 无 SCO
  uint16_t num_acl_packets = 32;
  uint16_t le_acl_mtu = 251; uint8_t le_num_packets = 8;
  uint8_t hci_version = 0x0B;               // 5.2
  uint16_t hci_revision = 0x0000; uint8_t lmp_version = 0x0B;
  uint16_t lmp_subversion = 0xFFFF; uint16_t manufacturer = 0xFFFF; // 0xFFFF=测试用
};
class MiniController {
 public:
  explicit MiniController(const ControllerConfig& cfg);
  // —— 主机侧（USB 适配器模型调用）——
  void handle_command(const uint8_t* p, size_t n);          // 入队事件应答
  void handle_acl_from_host(const AclFrame&);               // M3 用；M1 存日志
  bool pop_event(std::vector<uint8_t>& out);                // INT IN 拉取
  bool pop_acl_to_host(AclFrame& out);                      // BULK IN 拉取
  // —— 观测（测试/诊断）——
  std::vector<std::string> op_log() const;                  // "op=0x0C03(Reset) st=00"
  int unknown_opcodes_logged() const;
  uint32_t event_count() const;
  bool radio_ready() const;      // Reset 完成且收到过 Read_BD_ADDR 即置位（诊断口径）
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
```
**命令处理矩阵（实现依据；未列出的命令一律回 Command Complete{op, status=UnknownCmd} 并写 op_log（便于真机探针迭代补齐））：**

| 命令 | 应答 |
|---|---|
| Reset | CC{status=0}（清状态） |
| Read_BD_ADDR | CC{0, addr[6]} |
| Read_Local_Name | CC{0, 248B name+NUL} |
| Write_Local_Name | CC{0}（存） |
| Read_Buffer_Size | CC{0, acl_mtu LE, sco_mtu, sco_pkts, acl_pkts} |
| Read_Local_Version | CC{0, hci_version, hci_revision, lmp_version, manufacturer, lmp_subversion} |
| Read_Local_Supported_Commands | CC{0, 64B 位图：置位本文矩阵全部命令 + Host_Buffer_Size + Set_Event_Mask + Read_Country_Code(位) + Write_Page_Scan_Activity 等常见；位图以测试断言固定子集} |
| Read_Local_Supported_Features | CC{0, 8B：BR/EDR 支持、SSP(位13)、LE(位25=字节3 bit1)、3DH/5DH；不含 slot 换算} |
| Read_Local_Extended_Features | CC{0, page, max=1, 8B} |
| Set_Event_Mask / LE_Set_Event_Mask | CC{0}（存掩码；事件仍照发） |
| Host_Buffer_Size / Host_Num_Completed_Packets | CC{0}（流控记账可空实现） |
| Write_Scan_Enable/Inquiry_Mode/Page_Timeout/Class_Of_Device/Authentication_Enable/Simple_Pairing/Secure_Connections_Host_Support(echo 0)/Flow_Control_Mode/Page_Scan_Activity/Inquiry_Scan_Activity/Voice_Setting(0x0060)/PIN 码类 | CC{0} |
| Read/Write_Extended_Inquiry_Response | CC{0}[+240B] |
| Read_LE_Host_Support / Write_LE_Host_Support | CC{0}（回写值） |
| LE_Set_Event_Mask | CC{0} |
| LE_Read_Buffer_Size | CC{0, le_acl_mtu LE, 0x00(sco mtu), le_pkts}（**注意参数序：status, le_acl_mtu u16, le_sco_mtu u8, le_num_pkts u8**） |
| LE_Read_Local_Supported_Features | CC{0, 8B：LE 加密(0)、连接参数请求、扩展拒绝、扩展广播(1)、LE 2M(1)、稳定调制…字节0=0x01, 字节1=0x0F, 字节2..7=0（保守）} |
| LE_Read_White_List_Size | CC{0, 0x10} |
| LE_Clear_White_List / LE_Set_Host_Channel_Classification | CC{0} |
| LE_Set_Random_Address / LE_Set_Advertising_Parameters/Data/Enable / LE_Set_Scan_Parameters/Enable | CC{0}（M2 扩展行为；默认扫描关） |
| LE_Create_Connection / Cancel | CC{0}（M2 扩展） |
| LE_Set_Data_Length / Read_Suggested_Default_Data_Length(回 251,251? → {0,251,212,251,212}) / LE_Read_Maximum_Data_Length({0,251,212,251,212}) | CC{0,...} |
| Read_Country_Code | CC{0, 0x00} |
| Read_Default_Erroneous_Data_Reporting / Write_... | CC{0}（读回 0） |
| Delete_Stored_Link_Key / Read_Stored_Link_Key | CC{0,...}（空集） |
| Write_Remote_Amp_Endpoint...（AMP 类） | UnknownCmd |
| 任意 Vendor (0x3F) | UnknownCmd + op_log（**关键：避免 Windows 触发厂商固件下载路径**；描述符 VID 用非常见值） |

- [ ] **Step 1: 失败测试** `tests/unit/test_controller.cpp`：
```cpp
PB_TEST(controller_reset_bd_addr_radio_ready) {
  pb::hci::MiniController c(pb::hci::ControllerConfig{});
  uint8_t reset[] = {0x03,0x0C,0x00};
  c.handle_command(reset, 3);
  std::vector<uint8_t> e; PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], pb::hci::EVT_COMMAND_COMPLETE); PB_CHECK_EQ(e[3], 0x0C); PB_CHECK_EQ(e[4], 0x03);
  uint8_t readbd[] = {0x09,0x10,0x00};
  c.handle_command(readbd, 3); c.pop_event(e);
  PB_CHECK_EQ(e[7], 0x11); PB_CHECK_EQ(e[12], 0x66);      // BD_ADDR 6B
  PB_CHECK(c.radio_ready());
}
PB_TEST(controller_unknown_vendor_logged) {
  pb::hci::MiniController c(pb::hci::ControllerConfig{});
  uint8_t vs[] = {0x00,0xFC,0x00};                         // vendor opcode 0xFC00
  c.handle_command(vs, 3);
  std::vector<uint8_t> e; c.pop_event(e);
  PB_CHECK_EQ(e[0], pb::hci::EVT_COMMAND_COMPLETE);
  PB_CHECK_EQ(e[5], 0x01);                                 // status=UnknownCmd 在 return params 首字节
  PB_CHECK_EQ(c.unknown_opcodes_logged(), 1);
}
PB_TEST(controller_le_buffer_size_params) {
  pb::hci::MiniController c(pb::hci::ControllerConfig{});
  uint8_t le[] = {0x02,0x20,0x00};                         // LE_Read_Buffer_Size
  c.handle_command(le, 3); std::vector<uint8_t> e; c.pop_event(e);
  // 0E 06 01 02 20 00 FB 01 00 08  → mtu=0x00FB, sco=0? 按实现参数序断言
  PB_CHECK_EQ(e[6], 0xFB); PB_CHECK_EQ(e[7], 0x00);
  PB_CHECK_EQ(e[9], 8);
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（命令表 `std::unordered_map<uint16_t, Handler>`；所有事件经 `emit()` 入队；互斥保护）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe controller` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(hci): MiniController 初始化子集"`

### Task 16: 蓝牙适配器 USB 模型

**Files:**
- Create: `src/bluetooth/adapter_model.h`, `src/bluetooth/adapter_model.cpp`
- Test: `tests/unit/test_bt_adapter.cpp`（含 fake vhci 完整 HCI 初始化序列）

**Interfaces:**
```cpp
namespace pb::bt {
class AdapterModel : public pb::usb::IUsbDeviceModel {
 public:
  AdapterModel(const pb::hci::ControllerConfig& cfg, uint32_t busnum, uint32_t devnum);
  void attach_sink(pb::usb::IUsbModelSink* s);
  pb::hci::MiniController& controller();       // 控制命令直达（广播等）
  // IUsbDeviceModel：
  std::vector<uint8_t> on_control(const pb::usb::ControlSetup&, const std::vector<uint8_t>&) override;
  void on_out(uint8_t ep, std::vector<uint8_t> data) override;   // 0x02 bulk → ACL 帧（可能分片，按 PB 位重组）
  void on_in(uint8_t ep) override;            // 0x81 INT IN 拉 event；0x82 BULK IN 拉 ACL
  void on_detach() override;
  // 观测
  int acl_tx_frames() const; int evt_tx_count() const;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
```
**USB 形态（Task 8 的 descriptor builder 组装）：**
- 设备描述符：class=0xE0 subclass=0x01 protocol=0x01，bcdUSB=0x0200? 用 0x0110（FS 蓝牙惯例）——**取 0x0110**；bMaxPacketSize0=64。
- 配置 = 2 接口无 IAD（单功能）：if0 alt0 class E0/01/01：EP 0x81 INT IN 16/1ms + 0x02 BULK OUT 64 + 0x82 BULK IN 64；if1 alt0 class E0/01/01 无端点（SCO 空 alt）。
- **EP0 HCI 类请求**：`bmRequestType=0x20, bRequest=0x00`（wIndex 无关）：OUT data stage = HCI 命令包 → `controller().handle_command`；wLength 与 data 长度一致。旧形态 `bmRequestType=0xE0`（历史 bRequest 值）同样接受。其余 EP0 请求走标准处理（GET_DESCRIPTOR 等）。
- **事件通道**：`on_in(0x81)` 时若事件队列有包 → `complete_in(0x81, 0, evt)`；否则挂起（内部记录 pending），`handle_command` 产生事件时若 pending → 立即 complete_in（经 sink，注意跨线程：sink 回调由 usbip server 的连接线程发起，complete_in 线程安全——server 侧写互斥已有）。
- **ACL**：BULK OUT 数据流式到达 → 按 PB 标志重组完整 L2CAP 帧 → `handle_acl_from_host`；BULK IN：`on_in(0x82)` 拉一个完整 ACL 帧（M3 用）。帧切分 ≤64B（控制器 → 主机方向按 le_acl_mtu-4=247 切，M3）。

- [ ] **Step 1: 失败测试** `tests/unit/test_bt_adapter.cpp`：
```cpp
PB_TEST(bt_adapter_descriptors_match_spec) {
  pb::bt::AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  auto& exp = m.exported();
  PB_CHECK_EQ(exp.dev_class, 0xE0u); PB_CHECK_EQ(exp.dev_subclass, 1u); PB_CHECK_EQ(exp.dev_protocol, 1u);
  PB_CHECK_EQ(exp.num_interfaces, 2u);
}
PB_TEST(bt_adapter_hci_over_ep0_and_event_intr) {
  FakeSink sink;  // 同 hid 测试
  pb::bt::AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  // EP0 类请求携带 Reset 命令
  uint8_t setup[8] = {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00}; // wLength=3
  auto resp = m.on_control(pb::usb::parse_setup(setup), {0x03, 0x0C, 0x00});
  PB_CHECK(resp.empty());                                  // 无 IN 数据阶段
  m.on_in(0x81);                                           // 取事件
  PB_CHECK_EQ(sink.completes, 1);
  PB_CHECK_EQ(sink.data[0][0], pb::hci::EVT_COMMAND_COMPLETE);
}
PB_TEST(bt_adapter_full_init_sequence_reaches_ready) {
  FakeSink sink; pb::bt::AdapterModel m(pb::hci::ControllerConfig{}, 7, 1);
  m.attach_sink(&sink);
  auto send_cmd = [&](std::vector<uint8_t> cmd) {
    uint8_t su[8] = {0x20, 0x00, 0, 0, 0, 0, (uint8_t)cmd.size(), 0};
    m.on_control(pb::usb::parse_setup(su), cmd);
    m.on_in(0x81);                                          // 事件到达（FakeSink 收集）
  };
  send_cmd({0x03,0x0C,0x00});              // Reset
  send_cmd({0x09,0x10,0x00});              // Read_BD_ADDR
  send_cmd({0x01,0x10,0x00});              // Read_Local_Version
  send_cmd({0x05,0x10,0x00});              // Read_Buffer_Size
  send_cmd({0x02,0x10,0x00});              // Read_Local_Supported_Commands
  send_cmd({0x03,0x10,0x00});              // Read_Local_Supported_Features
  PB_CHECK(m.controller().radio_ready());
  PB_CHECK_EQ(sink.completes, 6);
  // 每个都是 Command Complete
  for (auto& d : sink.data) PB_CHECK_EQ(d[0], 0x0E);
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现** → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe bt_` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(bluetooth): USB 蓝牙适配器模型"`

### Task 17: M1 集成 + radio 探针

**Files:**
- Create: `tools/run-m1.ps1`；`src/probes/main.cpp` 增加 `radio` 模式；`src/control/commands.cpp` 增加 `load-bt` 命令（构造 AdapterModel，busid 复用 add）；`src/cli/main.cpp` 增加 `load-bt`
- Test: `tests/unit/test_commands.cpp` 追加 bt 命令测试

**radio 探针**：`winrt-probe.exe radio --wait-ms 15000` —— `Windows::Devices::Radios::Radio::GetRadiosAsync()`，找 `RadioKind::Bluetooth`，检查 `State == On`；输出 `{"result":"pass","name":"Phantom-BT Radio"}` / fail(reason=noradio|off)。**降级路径**：若 C++/WinRT 不可用，probes 提供 `--fallback-registry` 模式查 `HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_XXXX&PID_XXXX*\Driver == BTHUSB` 判定驱动绑定（脚本两种都跑，取 OR）。

`run-m1.ps1` 流程：构建 → serve → load-bt → attach → 等 3s → `winrt-probe radio`（+fallback-registry）→ 结果 JSON → detach。无管理员/usbip.exe → SKIP(3)。
**M1 验收补充断言（回环模式总能跑）**：fake-vhci 客户端完整模拟 Windows 初始化序列（Task 16 测试的 TCP 版），断言所有应答非 UnknownCmd——放 `tests/unit/test_usbip_server.cpp` 追加 `PB_TEST(server_bt_loopback_full_init)`：经 TCP import 蓝牙设备、EP0 发 12 条初始化命令、INT IN 收全事件、统计 op_log 无 unknown。

- [ ] **Step 1: 追加失败测试**（bt 命令 + 回环 init） → **Step 2: 构建** → **Step 3: 实现** → **Step 4: 运行** `powershell -File tools/run-m1.ps1 -SkipRealDevice`；单测全绿
- [ ] **Step 5: Commit** `git commit -am "feat(integration): M1 蓝牙无线电验收"`

---

## M2 BLE 发现

### Task 18: VirtualAir——虚拟空口与广播

**Files:**
- Create: `src/hci/le_air.h`, `src/hci/le_air.cpp`
- Test: `tests/unit/test_le_air.cpp`

**Interfaces:**
```cpp
namespace pb::hci {
struct AdvPayload { std::array<uint8_t,31> adv{}; uint8_t adv_len=0;
                    std::array<uint8_t,31> scan_rsp{}; uint8_t scan_rsp_len=0; };
class VirtualAdvertiser {     // 一个虚拟 BLE 外设的广播源
 public:
  VirtualAdvertiser(uint64_t addr /*LE: addr[5]..addr[0] 大端数值，静态随机 0xC0xxxx..*/,
                    std::string name, bool connectable = true);
  void set_adv_data(const std::vector<uint8_t>& raw31);        // 直接原始 31B
  void set_adv_data_name(std::string_view name, bool add_flags = true); // FLAGS 06 + 完整名
  void set_adv_data_manufacturer(uint16_t company, const std::vector<uint8_t>& payload); // iBeacon 风格
  void set_scan_response(std::string_view name);
  void set_connectable(bool); uint64_t address() const;
  const AdvPayload& payload() const;
 private: uint64_t addr_; bool connectable_ = true; AdvPayload pl_; std::string name_;
};
class VirtualAir {            // MiniController 持有；空口=内存
 public:
  void add(std::shared_ptr<VirtualAdvertiser> adv); void remove(uint64_t addr);
  // 主机侧扫描状态由 MiniController 驱动：
  void set_host_scanning(bool enabled, bool active);
  // tick：每次调用若主机在扫描且距上次 ≥ interval_ms（默认 128ms/源），生成一条报告事件入队
  void tick(uint64_t now_ms, std::function<void(std::vector<uint8_t> le_meta_event)> emit);
  // LE_Create_Connection：地址命中 connectable 广播源 → 返回该源（连接成功），否则 nullptr
  std::shared_ptr<VirtualAdvertiser> find_connectable(uint64_t addr);
  std::vector<std::shared_ptr<VirtualAdvertiser>> devices() const;
 private: /* ... */ };
}
```
**LE Advertising Report 事件（0x3E/0x02）格式**：`subevent(0x02), num_reports(1), evt_type, addr_type(0=public/1=random), addr[6], data_len, data[31], RSSI(0xC0=-64dBm)`。evt_type：ADV_IND=0x00（connectable+scannable）、ADV_NONCONN_IND=0x02、SCAN_RSP=0x04。iBeacon 厂商数据：`02 01 06 1A FF 4C 00 02 15 <UUID16> <major2> <minor2> <tx1>`（Apple 帧格式，company=0x004C）。

- [ ] **Step 1: 失败测试** `tests/unit/test_le_air.cpp`：
```cpp
PB_TEST(air_adv_report_event_layout) {
  pb::hci::VirtualAir air;
  auto d = std::make_shared<pb::hci::VirtualAdvertiser>(0xC01122334455ull, "PhantomKb");
  d->set_adv_data_name("PhantomKb"); d->set_scan_response("PhantomKb");
  air.add(d); air.set_host_scanning(true, true);
  std::vector<std::vector<uint8_t>> events;
  air.tick(0, [&](std::vector<uint8_t> e){ events.push_back(e); });
  PB_CHECK_EQ(events.size(), 1u);
  auto& e = events[0];
  PB_CHECK_EQ(e[0], 0x02); PB_CHECK_EQ(e[1], 1u);       // subevent, num
  PB_CHECK_EQ(e[2], 0x00);                               // ADV_IND
  PB_CHECK_EQ(e[3], 1u);                                 // random addr
  PB_CHECK_EQ(e[4], 0x55); PB_CHECK_EQ(e[9], 0xC0);      // addr LSB first
  // data: 02 01 06 + 完整名
  PB_CHECK_EQ(e[10], 0x02); PB_CHECK_EQ(e[11], 0x01); PB_CHECK_EQ(e[12], 0x06);
  PB_CHECK_EQ(e[13], 0x0B); PB_CHECK_EQ(e[14], 'P');     // len + 名
  int rssi_idx = 10 + e[10+0] /*粗略*/;                  // 测试按 data_len 字段精确定位
  // （写测试时按 e[10+e[10]+3] 索引 RSSI 处断言 0xC0）
}
PB_TEST(air_active_scan_yields_scan_rsp_second) {
  /* 同上，active=true；tick 两次：第 1 次后返回 ADV_IND+SCAN_RSP 两条（实现决策：
     每次 tick 对每个源产出 ADV_IND 与 SCAN_RSP 两事件，简化主机侧处理）→ 断言 2 条且第 2 条 evt_type=0x04 */
}
PB_TEST(air_ibeacon_payload) {
  auto d = std::make_shared<pb::hci::VirtualAdvertiser>(0xC0AABBCCDDEE, "beacon", false);
  d->set_adv_data_manufacturer(0x004C, {/*21B: 02 15 + uuid16 + major + minor + tx*/});
  auto& pl = d->payload();
  PB_CHECK_EQ(pl.adv[0], 3u); PB_CHECK_EQ(pl.adv[1], 3u);   // AD len=3, type=manufacturer
  PB_CHECK_EQ(pl.adv[2], 0x17);                              // 0xFF
  // （精确断言 company LE 4C 00）
}
PB_TEST(air_connect_lookup) {
  /* connectable=false 的源 find_connectable 返回 nullptr；true 的命中 */
}
```
- [ ] **Step 2: 构建**（失败） → **Step 3: 实现**（`set_adv_data_name`：flags AD(3B) + 完整本地名 AD(len+2) 截断 31B；tick 去重地址；每源独立 last_tick）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe air` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(hci): 虚拟空口与广播源"`

### Task 19: LE 扫描 HCI 面 + 广播控制命令 + M2 集成

**Files:**
- Modify: `src/hci/controller.cpp`（LE_Set_Scan_Enable 驱动 air；LE_Create_Connection/Cancel 接入 air 查找 → LE Connection_Complete；tick 由独立 100ms 定时线程驱动（simulator 拥有））
- Create: `src/ble/ble_device.h`（占位类型：`struct BleDeviceDef { uint64_t address; std::string name; bool connectable; std::string kind; /*keyboard|beacon|gatt*/ std::string path; }`），`profiles/ble_keyboard.json`, `profiles/ble_beacon.json`；`src/control/commands.cpp` 增加 `ble-advertise <json> / ble-stop <addr-hex> / ble-list`；`src/cli/main.cpp` 同步；`tools/run-m2.ps1`；probes 增加 `watcher` 模式
- Test: `tests/unit/test_controller.cpp` 追加扫描/连接测试

**LE 扫描/连接语义（controller 内实现）：**
- `LE_Set_Scan_Parameters` → CC{0}；`LE_Set_Scan_Enable(en, dup)` → CC{0}，en=1 置 air.set_host_scanning(true, active 由 scan type)；en=0 停。
- `LE_Create_Connection(...peer_addr...)` → CC{0}（status 事件）→ 200ms 后（tick 内一次性触发）若 air.find_connectable 命中：emit `LE Connection_Complete{status=0, handle=next++, role=0x00 central, peer_addr_type/addr, interval=24(30ms), latency=0, timeout=72(720ms? 500ms→40), master_acc=12, master_clk=0}`；未命中 → `LE Connection_Complete{status=0x02 UnknownConnId? 用 0x1F}`（Windows 语义：找不到设备）。
- `LE_Create_Connection_Cancel` → CC{0} + 若已 Complete 则幂等。
- 连接后：`HCI_Disconnect(handle, 0x13)` → CC{0} + `Disconnection_Complete`；（M3 扩展 ACL）。
- **tick 线程**：Simulator 启动 16ms 定时线程 → `MiniController::tick()` → air 扫描事件 + 连接延迟触发。

- [ ] **Step 1: 失败测试**（追加）：
```cpp
PB_TEST(controller_scan_enable_streams_adv_events) {
  pb::hci::MiniController c(cfg_with_test_air_and_device);   // helper: controller 构造时注入 air+设备
  // LE_Set_Scan_Enable(1)
  c.handle_command(cmd(0x0C20? …), …);  // 用 opcode(8,0x000C) params={01 00}
  drain_cc(c);                                            // 消费 Command Complete
  c.tick_for_tests(0); c.tick_for_tests(200);              // 时间推进
  std::vector<uint8_t> e; int adv = 0;
  while (c.pop_event(e)) if (e[0]==EVT_LE_META && e[1]==LE_EVT_ADVERTISING_REPORT) ++adv;
  PB_CHECK(adv >= 2);
}
PB_TEST(controller_le_create_connection_completes) {
  /* 空口含 connectable 设备；LE_Create_Connection(peer=该地址) → 消费 status → tick →
     LE_META/CONNECTION_COMPLETE status=0 handle=0x0001 role=central；再次 Create 另一地址 → status=0x1F */
}
```
（controller 接口追加测试钩子 `tick_for_tests(uint64_t now_ms)` 与 air 注入 `void attach_air(VirtualAir*)`——生产同样路径。）
- [ ] **Step 2: 构建** → **Step 3: 实现** → **Step 4: 运行** 单测全绿 + `powershell -File tools/run-m2.ps1 -SkipRealDevice`（回环：模拟扫描序列，断言事件流）
- **Step 5: Commit** `git commit -am "feat(hci,ble): LE 扫描与连接 HCI 面 + M2 集成"`

**watcher 探针**：`winrt-probe.exe watcher --duration-ms 8000 --expect-name PhantomKb`：`BluetoothLEAdvertisementWatcher{ScanningMode=Active}` + `Received` 处理器累计；结束时输出 `{"result":"pass","seen":[{"name":"PhantomKb","addr":"5544332211C0"}...]}`；命中 expect-name 退出 0，超时未命中 2。

---

## M3 BLE HID 全链路

### Task 20: LE 连接数据面（ACL 桥 + 连接管理）

**Files:**
- Create: `src/hci/link.h`, `src/hci/link.cpp`
- Modify: `src/hci/controller.cpp`（ACL 分发）
- Test: `tests/unit/test_link.cpp`

**Interfaces:**
```cpp
namespace pb::hci {
struct Link {
  uint16_t handle; uint64_t peer_addr; uint8_t peer_addr_type;
  bool encrypted = false; bool connected = true;
  // 主机→设备 ACL 交付：MiniController::handle_acl_from_host 剥 4B 头后整帧交给回调
  // 设备→主机：link_post_to_host(handle, l2cap_frame_bytes)
};
class LinkTable {
 public:
  uint16_t alloc(uint64_t peer, uint8_t type);             // 0x0001 起
  Link* find(uint16_t handle); Link* find_by_peer(uint64_t addr);
  void release(uint16_t handle);
  std::function<void(uint16_t handle, const std::vector<uint8_t>&)> on_host_to_device;  // 完整 L2CAP PDU
  std::function<void(const pb::hci::AclFrame&)> post_to_host;                            // controller 注入
};
}
```
**语义：**
- `MiniController::handle_acl_from_host`：按 PB 位重组（CONT 帧追加；新 START 帧重置）→ 完整 L2CAP PDU → `link_table.on_host_to_device(handle, pdu)`。
- 设备侧发送：`post_to_host` 组 ACL（PB=START_AUTO，长度 ≤ le_acl_mtu-4；>247 时分片 START+CONT）→ `pop_acl_to_host` 队列。
- 连接后自动事件：`LE Connection Update Complete`（可选）；`Disconnection Complete{handle, reason}`（release 时或 Disconnect 命令）。
- LL 控制过程**全部在 HCI 事件层伪造**（设计决策：两主机都在进程内，无需真实 LL PDU）：`LE_Read_Remote_Features`（主机发起）→ 100ms 后 `LE Read Remote Features Complete{status=0, handle, features[8]={0x01,0x0F,...}}`。

- [ ] **Step 1: 失败测试** `tests/unit/test_link.cpp`：
```cpp
PB_TEST(link_reassembly_host_to_device) {
  /* MiniController + air 设备 + LinkTable；建立连接得 handle=1；
     on_host_to_device 捕获 PDU；
     发两帧 ACL：START(前 4B l2cap 头) + CONT(剩余) → 捕获 1 个完整 PDU，内容一致 */
}
PB_TEST(link_device_to_host_fragmentation) {
  /* post_to_host 发 300B L2CAP PDU → pop_acl_to_host 得 2 帧：首帧 PB=START 247B 数据，次帧 PB=CONT 53B；
     两帧数据拼接 == 原 PDU */
}
PB_TEST(link_read_remote_features_event) {
  /* LE_Read_Remote_Features(handle) → CC → tick → LE_META 0x04 status=0 */
}
```
- [ ] **Step 2: 构建** → **Step 3: 实现** → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe link` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(hci): LE 链路数据面"`

### Task 21: L2CAP 固定通道 + ATT 服务器 + GATT 数据库

**Files:**
- Create: `src/ble/l2cap.h`, `src/ble/l2cap.cpp`, `src/ble/att.h`, `src/ble/att.cpp`, `src/ble/gatt_db.h`, `src/ble/gatt_db.cpp`
- Test: `tests/unit/test_l2cap.cpp`, `tests/unit/test_att.cpp`, `tests/unit/test_gatt_db.cpp`

**Interfaces:**
```cpp
namespace pb::ble {
class L2capFixedDemux {   // LE 固定通道：ATT CID=0x0004, SM CID=0x0006
 public:
  void attach(uint16_t handle, class AttServer* att, class SmpResponder* smp);
  void on_pdu(uint16_t handle, const std::vector<uint8_t>& pdu);   // 由 LinkTable 回调
  std::vector<uint8_t> build_pdu(uint16_t cid, const std::vector<uint8_t>& payload); // 4B 头
};
struct GattAttr {
  uint16_t handle; uint16_t type;             // 0x2800 svc decl / 0x2803 char decl / 0x2900..2908 descriptors / 0x2Axx 值类型
  uint8_t props; uint16_t value_type;         // char decl 指向的值 UUID16
  std::vector<uint8_t> value;                 // 服务声明值 = 16 位服务 UUID
  bool readable = true, writable = false;
  uint16_t min_len = 0, max_len = 512;
};
class GattDb {
 public:
  static GattDb build(const std::vector<GattAttr>& attrs);    // 校验句柄递增唯一
  const GattAttr* by_handle(uint16_t h) const;
  // 组查询返回 raw 属性字节（供 ATT 组装）：read_by_group_type / find_by_type 等
  std::vector<std::pair<uint16_t, const GattAttr*>> primary_services() const; // type==0x2800
  std::optional<std::vector<uint8_t>> read_value(uint16_t h, uint16_t offset, int& att_err);
  bool write_value(uint16_t h, const std::vector<uint8_t>& v, int& att_err);
  void set_value(uint16_t h, const std::vector<uint8_t>& v);   // 通知用（HOGP 报告）
  int mtu = 23;
};
class AttServer {
 public:
  explicit AttServer(GattDb& db);
  std::optional<std::vector<uint8_t>> handle(const uint8_t* pdu, size_t n); // 无应答（Write Cmd/通知）→ nullopt
  // ATT PDU 码：0x01 EReq 0x02 MTUReq 0x03 MTURsp 0x04 FindInfoReq 0x05 FindInfoRsp
  //  0x06 FindByTypeValReq 0x07 Rsp 0x08 ReadByTypeReq 0x09 Rsp 0x0A ReadReq 0x0B ReadRsp
  //  0x0C ReadBlobReq 0x0D Rsp 0x0E ReadMultiReq 0x0F Rsp 0x10 ReadByGroupReq 0x11 Rsp
  //  0x12 WriteReq 0x13 WriteRsp 0x16 PrepWriteReq 0x17 Rsp 0x18 ExecWriteReq 0x19 Rsp
  //  0x1D ReadMultiVarReq 0x1E Rsp 0x52 WriteCmd
  uint16_t peer_mtu() const;
 private: GattDb& db_;
};
}
```
**ATT 错误码**：InvalidHandle 0x01, ReadNotPermitted 0x02, WriteNotPermitted 0x03, InvalidPdu 0x04, InsufficientAuthn 0x05, UnsupportedReq 0x06, InvalidOffset 0x07, InsufficientAuthz 0x08, PrepQueueFull 0x09, AttrNotFound 0x0A, AttrNotLong 0x0B, InsufficientKeySize 0x0C, InvalidAttrLen 0x0D, UnlikelyErr 0x0E, InsufficientEnc 0x0F, UnsupportedGroupType 0x10, InsufficientResources 0x11。
**关键语义：**
- `ReadByGroupType(type=0x2800)`：返回每个服务 `{start, end, value}`，end = 下一服务 start-1（末服务 = 0xFFFF）；条目按 mtu-2 截断数据长（第一条定长）。
- `ReadByType(0x2803)`：char decl 列表 `{handle, props+value_handle LE+value_type}`；查询值类型（如 0x2A4B）返回 `{handle, value}` 按首条长度截断。
- `FindInformation`：返回 handle+UUID16 短格式（0x01）对；描述符发现（0x2902/0x2908）必需。
- `Read(handle)`：offset 0；`ReadBlob(offset)`：越界 0 字节；长值（Report Map ~65B）分块。
- `Write`：CCC(0x2902) 写 0100 → 订阅；`WriteCmd`（0x52）无应答。
- MTU Exchange：回 `MTURsp(min(our 247, peer))`，更新 db.mtu。
- 未知/畸形 PDU → `Error{req_opcode, handle, UnsupportedReq|InvalidPdu}`。

- [ ] **Step 1: 失败测试**（节选，全部按此风格写全）`tests/unit/test_att.cpp`：
```cpp
PB_TEST(att_mtu_exchange) { /* req 02 00F4 → rsp 03 00F4；db.mtu=247? 我们的 our=247，回 min */ }
PB_TEST(att_read_by_group_primary_services) {
  /* db: 0x0001 GAP svc(0x1800) … 0x0006 HID svc(0x1812) … 0x001A 末
     req: 10 01 00 FF FF 00 28 → rsp 11 len=20? 条目 20B{start,end,uuid}；断言第一条 01 00 05 00 00 18、第二条 06 00 19 00 12 18 */
}
PB_TEST(att_read_by_type_char_discovery) {
  /* req: 08 03 2A4A …（ReadByType 查 0x2A4A HID Info）→ 09 首字节长度 7 → value 01 01 00 02 */
}
PB_TEST(att_error_unknown_handle) {
  /* Read(0x0099) → 01 0A 99 00 01 */
}
PB_TEST(att_ccc_write_and_notify) { /* 写 0x2902=0100 成功；再写 0x0000 也成功 */ }
PB_TEST(att_blob_report_map) { /* Report Map 值 65B：ReadBlob(0)=22B？(mtu-1)；ReadBlob(22)=剩余 */ }
```
`tests/unit/test_l2cap.cpp`：
```cpp
PB_TEST(l2cap_demux_att_and_sm) { /* CID 0x0004 → AttServer；0x0006 → SmpResponder(占位)；其他 CID → 忽略 */ }
PB_TEST(l2cap_build_header) { /* build_pdu(4,{01,02}) == 03 00 04 00 01 02 */ }
```
- [ ] **Step 2: 构建** → **Step 3: 实现** → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe att` / `l2cap` / `gatt` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(ble): L2CAP/ATT/GATT 数据库"`

### Task 22: GATT 服务集（GAP/HOGP/Battery）+ BLE 键盘

**Files:**
- Create: `src/ble/services.h`, `src/ble/services.cpp`, `src/ble/hogp.h`, `src/ble/hogp.cpp`
- Modify: `profiles/ble_keyboard.json`
- Test: `tests/unit/test_services.cpp`, `tests/unit/test_hogp.cpp`

**Interfaces:**
```cpp
namespace pb::ble {
// 标准句柄布局（ble_keyboard.json 固定，测试 golden）：
// 0x0001 GAP svc(0x1800)｜0x0002/03 Device Name(0x2A00,R)｜0x0004/05 Appearance(0x2A01,R)
// 0x0006 HID svc(0x1812)｜0x0007/08 HID Info(0x2A4A,R)｜0x0009/0A Report Map(0x2A4B,R)
// 0x000B/0C Control Point(0x2A4C,W no-rsp)｜0x000D/0E Protocol Mode(0x2A4E,R+Wnr)
// 0x000F Report#1 decl(0x2A4D R+Notify)｜0x0010 value｜0x0011 ReportRef(0x2908,id=1,input)｜0x0012 CCC(0x2902)
// 0x0013 Report#2 decl(Wnr)｜0x0014 value｜0x0015 ReportRef(id=2,output)
// 0x0016 Battery svc(0x180F)｜0x0017/18 Battery Level(0x2A19 R+Notify)｜0x0019 CCC
// 0x001A 结束
struct ServiceSet { std::string device_name; uint16_t appearance = 0x03C1 /*Keyboard*/; };
std::vector<GattAttr> build_keyboard_attrs(const ServiceSet&);
class Hogp {   // HOGP 行为机
 public:
  Hogp(GattDb& db);
  void on_write(uint16_t handle, const std::vector<uint8_t>& v);  // control point/protocol mode/output report
  // 输入报告推送（控制台 → BLE 键盘）：需对应 CCC 已订阅（否则丢弃并计数）
  bool send_input_report(uint8_t report_id, const std::vector<uint8_t>& keys);
  std::function<void(std::vector<uint8_t> l2cap_pdu)> notify_post;  // ATT 通知 → L2CAP(0x0004) → link
  uint8_t protocol_mode() const; int dropped_reports() const;
};
}
```
`ble_keyboard.json`：
```json
{ "kind": "keyboard", "name": "PhantomKb", "address": "C0:11:22:33:44:55", "connectable": true,
  "hid_map": "boot_keyboard" }
```
`send_input_report` 载荷：标准键盘修饰键+保留+6 键码（8B）。`template_ble_keyboard_reportmap()` 复用 Task 10 键盘主集合字节（无 vendor 集合）。Report Map 值 = 该字节数组。
**Windows 侧预期交互顺序（集成断言依据，写进 hogp 测试）**：ReadByGroup → ReadByType(char) → Read(Report Map) → 写 Protocol Mode=report(0x01) → FindInfo(CCC) → 写 CCC=0100 → 此后 `send_input_report(1, {mod,0,k1..k6})` → 通知 0x1B `handle(0x0010 LE) + 8B`。

- [ ] **Step 1: 失败测试**：`test_services.cpp`（句柄布局逐项断言：0x0001 处 type=0x2800 value=00 18；0x0009 值声明 0x2A4B；0x0011 type=0x2908 value=01 01；0x0012 type=0x2902；Report Map = template 字节）；`test_hogp.cpp`（未订阅 send_input_report→false/dropped=1；订阅后 notify_post 收到 `1B 10 00 <8B>`；写 0x000C(控制点) suspend(0x00)/exit-suspend(0x01) 状态转移；写 0x000E=0x01 report 模式）。
- [ ] **Step 2: 构建** → **Step 3: 实现** → **Step 4: 运行** — passed
- [ ] **Step 5: Commit** `git commit -am "feat(ble): GAP/HOGP/Battery 服务与BLE键盘"`

### Task 23: SMP Just Works 配对（M3b）+ AES-CMAC

**Files:**
- Create: `src/ble/aes.h`, `src/ble/aes.cpp`, `src/ble/smp.h`, `src/ble/smp.cpp`
- Test: `tests/unit/test_aes.cpp`, `tests/unit/test_smp.cpp`

**Interfaces:**
```cpp
namespace pb::crypto {
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]); // FIPS-197
void aes_cmac(const uint8_t key[16], const uint8_t* msg, size_t n, uint8_t out[16]);     // RFC 4493
}
namespace pb::ble {
class SmpResponder {
 public:
  // LL 加密由进程内伪造（无真实空口），SMP 只需协议交换与密钥推导正确
  std::optional<std::vector<uint8_t>> handle(const uint8_t* pdu, size_t n); // CID 0x0006
  // 状态机：PairingReq→Rsp(NoInputNoOutput/JustWorks)→Confirm(c1, TK=0)→Random→（STK=s1）
  // LTK 请求（LL 层伪造后 MiniController 发 LE LTK Request 事件给谁？——注意：本设计 Windows 是
  // 主机侧 SMP 发起方；外设侧 SMP 应答是我们。加密启动：Windows 发 LE_Start_Encryption(rand,ediv,LTK=STK)
  // → MiniController 直接回 Encryption Key Refresh Complete（无需真实密钥一致性）
  const uint8_t* stk() const;  // s1(TK=0, Srand, Mrand)，供测试
  bool paired() const;
};
}
```
**公开测试向量（必须使用，防自证）：**
- FIPS-197 Appendix C.1：key `000102030405060708090A0B0C0D0E0F`, pt `00112233445566778899AABBCCDDEEFF` → ct `69C4E0D86A7B0430D8CDB78070B4C55A`。
- RFC 4493 §4 测试：K=`2B7E1516 28AED2A6 ABF71588 09CF4F3C`；Mlen=0 → `BB97D012 E1D0ED90 329A73DA 9653C78C`；Mlen=16（`6BC1BEE2...3172A`）→ `070A16B4 6B4D4144 F79BDD9D D04A287C`。
- s1 定义（Core Vol3 H 2.2.2）：`s1(k, r1, r2) = e(k, r1' ⊕ r2')`，r' = 取 r 低 64bit 置于低 64bit、高 64bit 清零。测试内独立构造期望值（调用 aes128_encrypt_block 直接算）与 SmpResponder 输出比对，另用 c1 定义独立复算 Confirm。

- [ ] **Step 1: 失败测试**：`test_aes.cpp`（上述向量逐字节）；`test_smp.cpp`（Req{03 00 00 07 10 07 02}（IO=3 OOB=0 auth=0x00 max=16 init=02? 保守用 auth=0x07（bonding|MITM=0））→ Rsp{03 00 00 03 02 02 02}? 实现取 io=3,auth=0,dist=0；Confirm(03 + c1 结果) → Confirm；Random(04 + 16B) → Random(05 + 我们的 Srand)；此后 paired()=true，stk() 与测试内独立 s1 复算一致）。
- [ ] **Step 2: 构建** → **Step 3: 实现**（AES: S-box/逆S-box 常量表 + 10 轮；CMAC 按 RFC 4493 子密钥生成）
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe aes` / `smp` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(ble): SMP Just Works 与 AES-CMAC"`

### Task 24: M3 全链路集成（回环 fake-host + 真机脚本）

**Files:**
- Create: `tools/run-m3.ps1`；probes 增加 `ble` 模式；`src/control/commands.cpp` 增加 `ble-connect-sim`（调试用：进程内直接触发 fake 主机流程）
- Test: `tests/integration/test_ble_e2e.cpp`（并入 phantombt-tests，标记 `e2e_ble_`）

**回环端到端测试**（不经内核，测 MiniController+ble 全栈）：进程内起 Simulator+BLE 设备（`ble-advertise profiles/ble_keyboard.json`）→ 用 `FakeHost`（测试内实现的迷你主机栈：直接调 controller 接口）执行：`LE_Set_Scan_Enable` 收广播 → `LE_Create_Connection` → 收 Connection Complete → ATT：MTU→ReadByGroup→ReadByType→Read ReportMap→写 ProtocolMode→FindInfo→写 CCC→等待 `send_input_report("hello")` 的通知 → 断言 8B 键盘报告逐字节正确 → Disconnect。
**真机段** `run-m3.ps1`：serve → load-bt → attach → `winrt-probe radio` → `winrt-probe watcher --expect-name PhantomKb` → `winrt-probe ble --address 5544332211C0 --expect-service 1812 --expect-hid-report --keys "hi"`（probe 内：FromBluetoothAddressAsync → GetGattServicesAsync 含 0x1812 → 找 HID 集合（HidDevice.GetDeviceSelector usagePage 1 usage 6，不限 vid/pid）→ 收到报告）→ detach。无权限 → SKIP(3)。
**M3b（配对）真机段**：`winrt-probe ble --pair`（DeviceInformation.Pairing.PairAsync，Just Works 无 UI）→ 断言 `ConnectionStatus==Connected` 且加密后 GATT 仍可读写。回环：FakeHost SMP 交换走 L2CAP CID 0x0006。

- [ ] **Step 1: 失败测试** `test_ble_e2e.cpp`（上述流程完整写全） → **Step 2: 构建** → **Step 3: 实现**（probe ble 模式 + 脚本） → **Step 4: 运行** `build\debug\tests\phantombt-tests.exe e2e_ble` + `powershell -File tools/run-m3.ps1 -SkipRealDevice` — passed
- [ ] **Step 5: Commit** `git commit -am "feat(integration): M3 BLE HID 端到端"`

---

## M4 GUI 与打磨

### Task 25: 命名管道控制 + Win32 GUI

**Files:**
- Create: `src/control/pipe_server.h`, `src/control/pipe_server.cpp`, `src/gui/main.cpp`, `src/gui/main_window.h`, `src/gui/main_window.cpp`, `src/gui/log_view.h`, `src/gui/log_view.cpp`
- Test: `tests/unit/test_pipe_server.cpp`（回环管道）

**Interfaces:**
```cpp
namespace pb::control {
class PipeServer {   // 管道名 \\.\pipe\phantom-bt；行协议 = execute() 文本
 public:
  explicit PipeServer(Simulator& sim);
  bool start(std::string* err); void stop();       // 每客户端线程；行读→execute→行写回
  // 扩展命令（本任务加入 commands.cpp）：
  //  log-follow          → 服务端持续推送日志行（sink 复用 PB_LOG），客户端断开即停
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
namespace pb::gui {
int run(Simulator& sim, HINSTANCE hInst, int nCmdShow);  // 消息循环；返回退出码
}
```
**GUI（纯 Win32，无资源文件，代码创建控件）：**
- 主窗口 900×640，Tab 控件三页：
  1. **设备**：ListView（列：busid/类型/VID:PID/名称/状态）+ 按钮：加载 HID Profile（GetOpenFileName）、加载蓝牙适配器、Attach、Detach、注入随机手柄按钮（调 inject 命令）。
  2. **BLE**：ListView（广播源）+ 按钮：加载 BLE Profile、开始广播、停止；Edit+按钮"键入文本"→ `ble-type <addr> <text>` 命令（Simulator 新增：逐键 send_input_report，60ms 间隔，按下+抬起两帧）。
  3. **日志**：只读 EDIT（ES_MULTILINE|WS_VSCROLL|ES_AUTOVSCROLL），后台线程经管道 `log-follow` 读行，`PostMessage(WM_APP_LOG)` 主线程追加（环形上限 5000 行）。
- GUI 进程 = 内嵌 Simulator + PipeServer（与 CLI serve 同构）；若管道已被占用（另一实例在跑）→ 弹窗提示"已有实例，将以客户端模式连接"→ GUI 直接调本地 execute 的函数指针改为经管道发送（`CommandClient` 小类：`std::vector<std::string> send(line)`）。
- 冒烟测试（`tests/unit/test_gui_smoke.cpp`，仅 Headless 可跑部分）：`CreateWindow` 主窗口类注册/创建/销毁不崩溃（不进入消息循环，不 Attach 输入）；`CommandClient` 经回环管道往返一条 `status`。

- [ ] **Step 1: 失败测试** `test_pipe_server.cpp`：
```cpp
PB_TEST(pipe_roundtrip_status) {
  pb::sim::Simulator sim; sim.start(&err);
  pb::control::PipeServer pipe(sim); PB_CHECK(pipe.start(&err));
  // 客户端：CreateFile("\\\\.\\pipe\\phantom-bt") → 写 "status\n" → 读到以 OK usbip-port 开头行
  auto lines = pb::control::CommandClient::connect_and_send("status");
  PB_CHECK(!lines.empty() && lines[0].rfind("OK usbip-port=", 0) == 0);
  pipe.stop(); sim.stop();
}
```
- [ ] **Step 2: 构建** → **Step 3: 实现**（`CreateNamedPipeA(PIPE_ACCESS_DUPLEX, BYTE|READMESSAGE? 用 BYTE_STREAM + \n 分帧)`；GUI CMake：`add_executable(phantombt-gui WIN32 ...)`)
- [ ] **Step 4: 运行** `build\debug\tests\phantombt-tests.exe pipe` 与 `gui_smoke` — passed；手工启动 GUI 冒烟（无头 CI 可跳过人工）
- [ ] **Step 5: Commit** `git commit -am "feat(gui,control): Win32 GUI 与命名管道"`

### Task 26: btsnoop 日志 + 覆盖率 + 文档 + 收尾

**Files:**
- Create: `src/sim/btsnoop.h`, `src/sim/btsnoop.cpp`, `tools/coverage.ps1`, `tools/run-tests.ps1`, `README.md`, `docs/troubleshooting.md`
- Test: `tests/unit/test_btsnoop.cpp`

**Interfaces:**
```cpp
namespace pb::sim {
class BtSnoopWriter {   // Wireshark btsnoop v1, Datalink=1002 (HCI UART H4)
 public:
  bool open(const std::string& path);               // 头: "btsnoop\0" + BE u32 1 + BE u32 1002
  void hci_event(const std::vector<uint8_t>& evt);  // 记录: H4 type 0x04 前缀
  void hci_command(const std::vector<uint8_t>& cmd);// 0x01
  void acl_from_host(const AclFrame&);              // 0x02
  void acl_to_host(const AclFrame&);                // 0x02
  uint64_t record_count() const;
 private: /* 文件句柄 + u64 BE 时间戳（btsnoop epoch 0x00E03AB1A6764000 + us） */
};
}
```
MiniController/AdapterModel 全部收发点挂钩 BtSnoopWriter（Simulator 持有，默认开 `integration-results/hci.snoop`，命令 `snoop <path>|off` 控制）。

`tools/run-tests.ps1`：构建 Release + 跑全部单测 + 各 run-m*.ps1 -SkipRealDevice，汇总退出码。
`tools/coverage.ps1`：定位 OpenCppCoverage（`winget install OpenCppCoverage.OpenCppCoverage` 提示安装）→ `OpenCppCoverage --sources src --excluded_sources src/gui --excluded_sources src/probes --excluded_sources src/cli -- build\\release\\tests\\phantombt-tests.exe` → HTML 报告 + 打印各模块行覆盖率；对 100% 目标模块产出达标/缺口清单。
`README.md`：架构图（文本）、前置条件（VS2026/WinSDK C++/WinRT 组件/usbip-win2 安装与 R-110：禁用内置蓝牙）、构建/测试/使用（CLI+GUI 命令表）、验收矩阵（M0-M3 对应探针命令）。
`docs/troubleshooting.md`：M1 失败排查法（op_log 全量、未知命令补齐流程）、多无线电问题、usbip attach 失败码表。

- [ ] **Step 1: 失败测试** `test_btsnoop.cpp`（头 16 字节 golden：`62 74 73 6E 6F 6F 70 00 00 00 00 01 00 00 03 EA`；写 1 条命令记录后文件长度 = 16 + 24(记录头) + 4(H4) + 3(Reset)；record_count()==1）→ **Step 2: 构建** → **Step 3: 实现** → **Step 4: 运行** `powershell -File tools/run-tests.ps1` 全绿；`tools/coverage.ps1` 出报告
- [ ] **Step 5: Commit** `git commit -am "feat(sim,tools): btsnoop 日志、覆盖率与文档"`

---

## 自审记录（writing-plans Self-Review）

1. **Spec 覆盖**：R2'(M0: T7-T13)、R3'(M1: T14-T17)、R4'(M2/M3: T18-T24)、R5'(winrt-probe: T13/17/19/24)、R7(CLI/GUI: T13/25；覆盖: T2/T26)、R6 继承（异常注入=HID STALL/断开路径 T11-T12、全量日志=T12 debug + T26 snoop、热切换=load+detach 组合）——全覆盖。
2. **占位符扫描**：T18/T19 等含"…（写测试时按 X 精确断言）"均为**测试内计算指引**而非实现留白；无 TBD/TODO。
3. **类型一致性**：`IUsbModelSink::complete_in(uint8_t,int32_t,std::vector<uint8_t>)`（T9/T11/T12/T16）；`execute(Simulator&,line)`（T13/T25）；`HidProfile::from_json(const json::Value&)`（T10/T11）；`MiniController::tick_for_tests/attach_air`（T19 定义、T24 使用）——一致。
4. 已知风险内嵌：T17 的"回环 M1 断言"把 R-108 的真机不确定性隔离为独立脚本段；T15 未知命令全量日志提供迭代闭环。

## 执行方式

用户已预先选择"一步到位"（inline 连续执行）：按 Task 1→26 顺序，每任务 TDD 循环 + 提交；集成真机段无权限则记录 SKIP 与人工步骤，最终统一汇报。

