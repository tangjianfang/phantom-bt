// bug_probe-skip：serve --probe 的假绿缺陷。
// 缺陷（修复前）：--probe 给出但探针未实际运行（无 --attach / 无已加载设备 /
// 无 usbip.exe）时，serve 仍以退出码 0 结束且无任何显式标记 → 验证脚本把
// “未验证”当“通过”。正确行为：探针不可运行时必须显式输出 SKIPPED 并以
// 非零码退出（与探针自身约定一致：0=pass 2=fail 3=skip，probes/main.cpp:8）。
// 经 testrunner::run_capture 拉起真实 phantombt.exe 子进程断言其输出与退出码。
#include "test/test_framework.h"

#include <windows.h>

#include <string>
#include <vector>

#include "cli/serve_args.h"
#include "core/bytes.h"
#include "core/env_check.h"
#include "core/test_runner.h"

using namespace pb;

namespace {

// 定位 CLI 可执行文件：测试 exe 在 build/<cfg>/tests/，phantombt.exe 在
// build/<cfg>/src/（CMake 布局，同 test_runner.cpp find_tests_exe 的候选式）。
std::string find_cli_exe() {
  std::wstring dir = pb::env::exe_dir();
  if (dir.empty()) return "";
  const wchar_t* kCandidates[] = {
      L"..\\src\\phantombt.exe",
      L"phantombt.exe",
      L"..\\..\\debug\\src\\phantombt.exe",
  };
  for (auto* c : kCandidates) {
    std::wstring full = dir + c;
    if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) return pb::from_w(full);
  }
  return "";
}

struct ServeRun {
  int rc = -1;
  std::vector<std::string> lines;
};

// 拉起 phantombt serve（端口固定 3425，本仓库真机/usbip 验证约定端口）。
// 探针命令取 "cmd.exe /c exit 7"：若被错误执行，serve 退出码会变成 7，
// 恰好与“跳过”码 3 区分。
ServeRun run_serve_probe(const std::string& extra_flags) {
  ServeRun r;
  std::string exe = find_cli_exe();
  if (exe.empty()) return r;
  std::string cmd = "\"" + exe + "\" serve --port 3425 --probe \"cmd.exe /c exit 7\" " +
                    extra_flags + " --exit-after-ms 300";
  r.rc = testrunner::run_capture(cmd, [&](const std::string& l) { r.lines.push_back(l); },
                                 nullptr, 30000);
  return r;
}

bool has_line_containing(const std::vector<std::string>& lines, const char* needle) {
  for (auto& l : lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

// 无 --attach：探针无法运行 → 必须显式 SKIPPED + 非零退出码（=3），
// 而不是修复前的退出码 0 假绿
PB_TEST(bug_probe_skip_no_attach_outputs_skipped_and_nonzero) {
  auto r = run_serve_probe("");
  PB_CHECK(r.rc != -1);                    // 子进程成功拉起（exe 在场）
  PB_CHECK_EQ(r.rc, 3);                    // skip 码；若探针被错误执行则为 7
  PB_CHECK(has_line_containing(r.lines, "SKIPPED"));
}

// 有 --attach 但未加载任何设备（busids 为空）：同样必须 SKIPPED + 非零退出码
PB_TEST(bug_probe_skip_attach_without_loaded_device_skips) {
  auto r = run_serve_probe("--attach");
  PB_CHECK(r.rc != -1);
  PB_CHECK_EQ(r.rc, 3);
  PB_CHECK(has_line_containing(r.lines, "SKIPPED"));
}

// ===================== bug cli-stoi（本工单）=====================
// CLI serve 的 --port/--exit-after-ms 缺值/非法值：安全解析（src/cli/serve_args.h）
// 必须把它变成"用法错误 + 非零退出码"，绝不抛 std::stoi 的 invalid_argument /
// out_of_range（未捕获异常会触发 Windows 错误报告模态框）。

// —— 纯解析：缺值（取值选项位于末尾）→ 用法错误，error 点名该选项 ——
PB_TEST(bug_cli_stoi_missing_value_is_usage_error) {
  auto p = cli::parse_serve_args({"--port"});
  PB_CHECK(!p.ok);
  PB_CHECK(p.error.find("--port") != std::string::npos);
  auto e = cli::parse_serve_args({"--exit-after-ms"});
  PB_CHECK(!e.ok);
  PB_CHECK(e.error.find("--exit-after-ms") != std::string::npos);
  // 其余取值选项同样不许缺值（此前 --load 缺值会静默压入空串）
  PB_CHECK(!cli::parse_serve_args({"--load"}).ok);
  PB_CHECK(!cli::parse_serve_args({"--ble"}).ok);
  PB_CHECK(!cli::parse_serve_args({"--probe"}).ok);
}

// —— 纯解析：非法值（空串/非数字/带尾巴/符号）→ 用法错误 ——
PB_TEST(bug_cli_stoi_invalid_value_is_usage_error) {
  const char* kBad[] = {"", "abc", "34x25", "1e3", "-5", "+5", " 42"};
  for (auto* s : kBad) {
    auto p = cli::parse_serve_args({"--port", s});
    PB_CHECK(!p.ok);
    PB_CHECK(p.error.find("invalid --port") != std::string::npos);
    auto e = cli::parse_serve_args({"--exit-after-ms", s});
    PB_CHECK(!e.ok);
    PB_CHECK(e.error.find("invalid --exit-after-ms") != std::string::npos);
  }
}

// —— 纯解析：越界（--port>65535、--exit-after-ms>INT_MAX）→ 用法错误 ——
// （std::stoi 对这类输入抛 out_of_range —— 正是被修掉的未捕获异常路径）
PB_TEST(bug_cli_stoi_out_of_range_is_usage_error) {
  PB_CHECK(!cli::parse_serve_args({"--port", "65536"}).ok);
  PB_CHECK(!cli::parse_serve_args({"--port", "99999999999999999999"}).ok);
  PB_CHECK(!cli::parse_serve_args({"--exit-after-ms", "2147483648"}).ok);
  PB_CHECK(!cli::parse_serve_args({"--exit-after-ms", "99999999999999999999"}).ok);
}

// —— 纯解析：合法值与既有选项语义不变 ——
PB_TEST(bug_cli_stoi_valid_values_and_flags_unchanged) {
  auto a = cli::parse_serve_args({"--port", "3425", "--exit-after-ms", "100"});
  PB_CHECK(a.ok);
  PB_CHECK_EQ(a.port, 3425);
  PB_CHECK_EQ(a.exit_after_ms, 100);
  // 默认值不变
  auto d = cli::parse_serve_args({});
  PB_CHECK(d.ok);
  PB_CHECK_EQ(d.port, 3240);
  PB_CHECK_EQ(d.exit_after_ms, 0);
  // --port 0 沿用"随机端口"语义（selftest 同款），未被新校验误伤
  auto z = cli::parse_serve_args({"--port", "0"});
  PB_CHECK(z.ok);
  PB_CHECK_EQ(z.port, 0);
  // 边界值合法
  PB_CHECK(cli::parse_serve_args({"--port", "65535"}).ok);
  PB_CHECK(cli::parse_serve_args({"--exit-after-ms", "2147483647"}).ok);
  // 文件/字符串选项原样透传；开关照旧
  auto f = cli::parse_serve_args(
      {"--load", "a.json", "--load-bt", "--ble", "b.json", "--attach", "--probe", "x y"});
  PB_CHECK(f.ok);
  PB_CHECK_EQ(f.loads.size(), 1u);
  PB_CHECK_EQ(f.loads[0], std::string("a.json"));
  PB_CHECK(f.load_bt && f.attach);
  PB_CHECK_EQ(f.bles.size(), 1u);
  PB_CHECK_EQ(f.bles[0], std::string("b.json"));
  PB_CHECK_EQ(f.probe, std::string("x y"));
  // 未知 flag 仍是用法错误（error 文案沿用 "unknown flag"）
  auto u = cli::parse_serve_args({"--bogus"});
  PB_CHECK(!u.ok);
  PB_CHECK(u.error.find("unknown flag") != std::string::npos);
}

// —— exe 层：缺值/非法值 → stderr 输出 usage、退出码 2（不崩、不挂模态框）——
// run_capture 合并 stdout+stderr（test_runner.cpp:89-90），在捕获行里找
// "usage:"。5 个用例都在参数检查处立即 return 2，无服务器启动、无副作用。
PB_TEST(bug_cli_stoi_exe_prints_usage_and_exits_nonzero) {
  std::string exe = find_cli_exe();
  PB_CHECK(!exe.empty());  // 同一构建树必在场（CMake 布局）
  const char* kCases[] = {"--port",           "--exit-after-ms", "--port abc",
                          "--port 1e3",       "--exit-after-ms 2147483648"};
  for (auto* c : kCases) {
    ServeRun r;
    std::string cmd = "\"" + exe + "\" serve " + c;
    r.rc = testrunner::run_capture(cmd, [&](const std::string& l) { r.lines.push_back(l); },
                                   nullptr, 30000);
    PB_CHECK_EQ(r.rc, 2);                              // 非零退出码
    PB_CHECK(has_line_containing(r.lines, "usage:"));  // 用法信息
    PB_CHECK(has_line_containing(r.lines, "[--exit-after-ms N]"));
  }
}
