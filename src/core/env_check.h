#pragma once
// 环境体检：GUI 首启 / CLI doctor 共用
#include <cstdint>
#include <string>
#include <vector>

namespace pb::env {

enum class Status : int { Ok = 0, Warn = 1, Fail = 2, Info = 3 };

struct CheckResult {
  std::string id;         // os / usbip_exe / usbip_driver / admin / probe / builtin_bt / port
  std::string title;      // UI 标题（中文）
  Status status = Status::Ok;
  std::string detail;     // 结果说明（版本号/路径/指引）
  std::string action_url;     // 非空 → 显示「打开官方页面」按钮
  std::string action_label;   // 按钮文字
};

// 官方地址
std::string usbip_download_url();     // usbip-win2 releases
std::string usbip_docs_url();

// —— 纯逻辑（可测） ——
bool os_build_ok(uint32_t build_number);              // ≥ Win10 1903 (18362)
bool looks_like_bt_enum_key(const std::string& key,   // VID_0A12&PID_0001 → true（蓝牙厂商 VID 集）
                            const std::string& our_vid /*"VID_0B00"*/);
std::string classify_os(uint32_t build, uint32_t major);

// —— 真机检测（进程内直查） ——
uint32_t windows_build();                    // RtlGetVersion
bool is_admin();                             // 当前进程管理员
bool service_exists(const char* name);       // SCManager 查询（如 usbip2_ude）
std::string find_usbip_exe();                // PATH + 常见安装位
bool file_next_to_exe(const char* name);     // 可执行文件同目录探测（winrt-probe.exe）
bool builtin_bluetooth_present(const std::string& our_vid);  // registry Enum\USB BT-VID scan
bool port_listened_by_other(uint16_t port);  // listener-table owner PID != this process
                                               // (the GUI's embedded server listening doesn't
                                               //  count, avoiding a false first-run warning)

// 汇总（GUI/CLI 用）
std::vector<CheckResult> run_checks();
// 是否存在需用户处理的项（Fail/Warn）
bool needs_attention(const std::vector<CheckResult>& checks);
// 首启标记（HKCU\Software\PhantomBT）
bool first_run_done();
void mark_first_run();

}  // namespace pb::env
