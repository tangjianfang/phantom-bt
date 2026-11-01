// 第 1 轮补覆盖：src/core/{bt_radio,env_check,net,json,log}.cpp 的未覆盖行。
// 全部测试确定性：不 sleep、不依赖时序/网络（仅本机回环）、不触碰物理硬件；
// 注册表/文件操作先备份、后还原。
#include "test/test_framework.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>  // 必须先于 windows.h
#include <windows.h>

#include "core/env_check.h"
#include "core/json.h"
#include "core/log.h"
#include "core/net.h"

using namespace pb;

// ============================ env_check ============================

// env_check.cpp:26 — usbip_docs_url 此前从未被调用。
PB_TEST(cov_core_env_urls) {
  PB_CHECK(pb::env::usbip_docs_url().rfind("https://", 0) == 0);
  PB_CHECK(pb::env::usbip_docs_url().find("usbip-win2") != std::string::npos);
}

// env_check.cpp:134-137 — 两个硬编码安装位之外的 PATH 回退扫描。
// 结果依赖机器是否装了 usbip.exe，只断言形状（空 或 含路径分隔符的绝对路径）。
PB_TEST(cov_core_env_find_usbip_exe_shape) {
  std::string p = pb::env::find_usbip_exe();
  if (!p.empty()) PB_CHECK(p.find('\\') != std::string::npos);
  PB_CHECK(p.empty() || p.size() < 260u);
}

// env_check.cpp:193-233 — 直查 HKLM\Enum\USB（只读）。两次调用结果必须一致；
// 覆盖 196（打开 Enum\USB）与 201（枚举耗尽 break）等行。
PB_TEST(cov_core_env_builtin_bt_probe_stable) {
  bool a = pb::env::builtin_bluetooth_present("VID_0B00");
  bool b = pb::env::builtin_bluetooth_present("VID_0B00");
  PB_CHECK_EQ(a, b);
}

// env_check.cpp:258-366 — run_checks 全量形状。各分支取值依赖机器状态，
// 只断言不变量：7 项、id 顺序、title/detail 非空、Fail 时动作链接正确、
// os 状态与真实 build 一致（本机 usbip.exe 缺失时还会走 287-290 的 Fail 分支）。
PB_TEST(cov_core_env_run_checks_full_shape) {
  auto checks = pb::env::run_checks();
  PB_CHECK_EQ(checks.size(), 7u);
  static const char* kIds[] = {"os", "usbip_exe", "usbip_driver", "admin", "probe", "builtin_bt", "port"};
  for (size_t i = 0; i < checks.size(); ++i) {
    PB_CHECK_EQ(checks[i].id, std::string(kIds[i]));
    PB_CHECK(!checks[i].title.empty());
    PB_CHECK(!checks[i].detail.empty());
  }
  // 2) 客户端工具缺失 → Fail 时必须带官方下载链接
  const auto& exe = checks[1];
  if (exe.status == pb::env::Status::Fail) {
    PB_CHECK_EQ(exe.action_url, pb::env::usbip_download_url());
    PB_CHECK(!exe.action_label.empty());
  }
  // 3) 驱动服务缺失 → Fail 时必须带安装文档链接
  const auto& drv = checks[2];
  if (drv.status == pb::env::Status::Fail) {
    PB_CHECK_EQ(drv.action_url, pb::env::usbip_docs_url());
    PB_CHECK(!drv.action_label.empty());
  }
  // 1) os 状态必须与真实 build 对应
  bool ok_expected = pb::env::os_build_ok(pb::env::windows_build());
  PB_CHECK_EQ((int)checks[0].status,
              (int)(ok_expected ? pb::env::Status::Ok : pb::env::Status::Fail));
}

// env_check.cpp:331-334 — 探针文件存在分支。API 只探测"测试进程 exe 所在目录"
// （GetModuleFileName），无法重定向，故在 exe 旁临时创建空 winrt-probe.exe、
// 调 run_checks 验证后立刻删除，恢复原状。若目录只读则退回 Warn 分支断言。
PB_TEST(cov_core_env_probe_file_next_to_exe) {
  std::wstring dir = pb::env::exe_dir();
  PB_CHECK(!dir.empty());
  std::wstring probe = dir + L"winrt-probe.exe";
  bool preexisting = GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES;
  bool created = false;
  if (!preexisting) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, probe.c_str(), L"wb") == 0 && f) {
      created = true;
      std::fclose(f);
    }
  }
  auto checks = pb::env::run_checks();
  const pb::env::CheckResult* pr = nullptr;
  for (auto& c : checks)
    if (c.id == "probe") pr = &c;
  PB_CHECK(pr != nullptr);
  bool present = preexisting || created;
  if (present) {
    PB_CHECK_EQ((int)pr->status, (int)pb::env::Status::Ok);      // 332-333
    PB_CHECK_EQ(pr->detail, std::string("就绪"));
  } else {
    PB_CHECK_EQ((int)pr->status, (int)pb::env::Status::Warn);    // 335-336
  }
  if (created) {
    DeleteFileW(probe.c_str());
    PB_CHECK(!pb::env::file_next_to_exe("winrt-probe.exe"));  // 还原机器状态
  }
}

namespace {
// HKCU\Software\PhantomBT 下单个值的完整备份（名字/类型/数据）。
struct RegValueBlob {
  std::string name;
  DWORD type = 0;
  std::vector<BYTE> data;
};
}  // namespace

// env_check.cpp:375-385 / 387-395 — first_run_done + mark_first_run 的真注册表
// 往返。先备份键下全部值，再经历「键缺失→false」「标记→true」，最后精确还原，
// 结尾断言状态与进入前一致。
PB_TEST(cov_core_env_first_run_roundtrip) {
  const char* kKey = "Software\\PhantomBT";
  const bool orig = pb::env::first_run_done();  // 覆盖 375/377/379/380/383/384

  // —— 备份 ——（blocked = 键里有子键/读不全的值 → 不整键删除，只动 FirstRunDone）
  bool key_exists = false, blocked = false;
  std::vector<RegValueBlob> backup;
  HKEY k = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
    key_exists = true;
    for (DWORD i = 0;; ++i) {
      char name[256];
      DWORD nlen = sizeof(name);
      DWORD type = 0;
      std::vector<BYTE> data(4096);
      DWORD dlen = (DWORD)data.size();
      LONG rc = RegEnumValueA(k, i, name, &nlen, nullptr, &type, data.data(), &dlen);
      if (rc == ERROR_NO_MORE_ITEMS) break;
      if (rc != ERROR_SUCCESS) {
        blocked = true;
        break;
      }
      backup.push_back({std::string(name, nlen), type,
                        std::vector<BYTE>(data.begin(), data.begin() + dlen)});
    }
    char sub[8];
    DWORD slen = sizeof(sub);
    if (RegEnumKeyExA(k, 0, sub, &slen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
      blocked = true;
    RegCloseKey(k);
  }

  // —— 键缺失分支（378）——
  if (!blocked) {
    LSTATUS rc = RegDeleteTreeA(HKEY_CURRENT_USER, kKey);
    PB_CHECK(rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    PB_CHECK(!pb::env::first_run_done());  // RegOpenKeyEx 失败 → false
  } else {
    HKEY kk = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &kk) == ERROR_SUCCESS) {
      RegDeleteValueA(kk, "FirstRunDone");  // 值本就不存在时失败也无妨
      RegCloseKey(kk);
    }
    PB_CHECK(!pb::env::first_run_done());  // 键在、值无 → false
  }

  // —— 标记 + 读回（387-390、392-395）——
  pb::env::mark_first_run();
  PB_CHECK(pb::env::first_run_done());

  // —— 还原 ——
  if (!key_exists) {
    RegDeleteTreeA(HKEY_CURRENT_USER, kKey);  // 机器本来就没有这个键
  } else if (!blocked) {
    HKEY kk = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &kk,
                        nullptr) == ERROR_SUCCESS) {
      bool had_flag = false;
      for (auto& v : backup) {
        if (v.name == "FirstRunDone") had_flag = true;
        RegSetValueExA(kk, v.name.c_str(), 0, v.type, v.data.data(), (DWORD)v.data.size());
      }
      if (!had_flag) RegDeleteValueA(kk, "FirstRunDone");
      RegCloseKey(kk);
    }
  } else {
    HKEY kk = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &kk) == ERROR_SUCCESS) {
      const RegValueBlob* flag = nullptr;
      for (auto& v : backup)
        if (v.name == "FirstRunDone") flag = &v;
      if (flag)
        RegSetValueExA(kk, "FirstRunDone", 0, flag->type, flag->data.data(), (DWORD)flag->data.size());
      else
        RegDeleteValueA(kk, "FirstRunDone");
      RegCloseKey(kk);
    }
  }
  PB_CHECK_EQ(pb::env::first_run_done(), orig);  // 状态已还原
}

// ============================ net ============================

// net.cpp:41-43 — 域名解析失败。".invalid" 保留 TLD 不会解析；即使被 DNS 劫持
// 意外解析，connect 也会失败，断言在两种路径下都成立。
PB_TEST(cov_core_net_connect_unresolvable_host) {
  net::WsaSession wsa;
  std::string err;
  auto c = net::TcpConn::connect("no-such-host.invalid", 54321, &err);
  PB_CHECK(!c.is_open());
  PB_CHECK(!err.empty());
}

// net.cpp:74-76 / 88-90 — 已关闭连接上的 send_all / recv_exact 错误分支。
// s_ 为无效句柄时 send/recv 立即返回 WSAENOTSOCK，无时序依赖。
PB_TEST(cov_core_net_closed_socket_send_recv_errors) {
  net::WsaSession wsa;
  net::TcpConn c;
  PB_CHECK(!c.is_open());
  uint8_t buf[4] = {};
  std::string err;
  PB_CHECK(!c.send_all(buf, sizeof(buf), &err));  // 74-76
  PB_CHECK(err.find("send") != std::string::npos);
  err.clear();
  PB_CHECK(!c.recv_exact(buf, sizeof(buf), &err));  // 88-90
  PB_CHECK(err.find("recv") != std::string::npos);
}

// net.cpp:102-111 — recv_some：正常收数据(109)、对端关闭 n_out=0(109)、
// 无效句柄错误(104-107)。TCP 保证数据先于 FIN 到达，无需 sleep。
PB_TEST(cov_core_net_recv_some_paths) {
  net::WsaSession wsa;
  {
    net::TcpListener l;
    std::string err;
    PB_CHECK(l.listen(0, &err));
    std::thread srv([&] {
      std::string e2;
      auto c = l.accept(&e2);
      c.send_all((const uint8_t*)"hey", 3, &e2);
      c.close();
    });
    auto c = net::TcpConn::connect("127.0.0.1", l.port(), &err);
    PB_CHECK(c.is_open());
    uint8_t buf[16] = {};
    int total = 0;
    while (total < 3) {
      int n = -1;
      PB_CHECK(c.recv_some(buf, sizeof(buf), &n, &err));
      PB_CHECK(n > 0);
      total += n;
    }
    PB_CHECK_EQ(total, 3);
    int n = -1;
    PB_CHECK(c.recv_some(buf, sizeof(buf), &n, &err));  // FIN 已排空 → 0
    PB_CHECK_EQ(n, 0);
    srv.join();
  }
  {  // 无效句柄 → 错误分支
    net::TcpConn closed;
    uint8_t buf[4] = {};
    int n = -1;
    std::string err;
    PB_CHECK(!closed.recv_some(buf, sizeof(buf), &n, &err));  // 104-107
    PB_CHECK(!err.empty());
  }
}

// net.cpp:141-144 — bind 冲突分支。TcpListener 固定设置 SO_REUSEADDR（Windows
// 下可抢占端口），普通双绑定打不进该分支；用 SO_EXCLUSIVEADDRUSE 的裸 socket
// 占住端口后，REUSEADDR 的第二次 bind 必得 WSAEACCES（微软文档化语义）。
PB_TEST(cov_core_net_listen_bind_exclusive_conflict) {
  net::WsaSession wsa;
  // 先借一个空闲端口并释放（未发生连接，无 TIME_WAIT 残留）
  net::TcpListener probe;
  std::string e0;
  PB_CHECK(probe.listen(0, &e0));
  uint16_t port = probe.port();
  probe.close();

  SOCKET holder = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  PB_CHECK(holder != INVALID_SOCKET);
  BOOL excl = TRUE;
  PB_CHECK(::setsockopt(holder, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl,
                        sizeof(excl)) == 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  PB_CHECK(::bind(holder, (sockaddr*)&a, sizeof(a)) == 0);

  net::TcpListener second;
  std::string err;
  PB_CHECK(!second.listen(port, &err));  // 142-144
  PB_CHECK(err.find("bind") != std::string::npos);

  ::closesocket(holder);
}

// ============================ json ============================

// json.cpp:80-85 — \/ \b \f \r \t 五个未覆盖的转义分支。
PB_TEST(cov_core_json_escape_control_chars) {
  auto v = pb::json::parse(R"({"s":"\/\b\f\r\t"})");
  PB_CHECK_EQ(v.find("s")->as_str(), std::string("/\b\f\r\t"));
}

// json.cpp:95 / 107 / 114 / 171 / 186 — 各类语法错误的 fail 路径。
PB_TEST(cov_core_json_bad_escapes_throw) {
  PB_CHECK_THROWS(pb::json::parse("\"\\q\""));           // 114 bad escape
  PB_CHECK_THROWS(pb::json::parse("\"\\uZZZZ\""));       // 95 主 \u 十六进制非法
  PB_CHECK_THROWS(pb::json::parse("\"\\uD83D\\uZZZZ\""));  // 107 低位代理非法
  PB_CHECK_THROWS(pb::json::parse("\"\\u1\""));          // \u 不足 4 位
  PB_CHECK_THROWS(pb::json::parse("{\"a\":1 \"b\":2}")); // 171 缺 ','/'}'
  PB_CHECK_THROWS(pb::json::parse("[1 2]"));             // 186 缺 ','/']'
}

// json.cpp:94 — \uXXXX 大写十六进制分支（既有测试只用了小写/数字）。
PB_TEST(cov_core_json_unicode_hex_uppercase) {
  PB_CHECK_EQ(pb::json::parse("\"\\u4E2D\"").as_str(), std::string("\xe4\xb8\xad"));
  PB_CHECK_EQ(pb::json::parse("\"\\u4e2d\"").as_str(), std::string("\xe4\xb8\xad"));
}

// json.cpp:98-111、134-137 — 代理对合成 + 4 字节 UTF-8。两例分别覆盖低位代理
// 的大写(106)与小写(105)十六进制。
PB_TEST(cov_core_json_surrogate_pairs) {
  auto up = pb::json::parse(R"({"e":"\uD83D\uDE00"})");  // U+1F600
  PB_CHECK_EQ(up.find("e")->as_str(), std::string("\xF0\x9F\x98\x80"));
  auto lo = pb::json::parse(R"({"e":"\ud83d\ude0a"})");  // U+1F60A
  PB_CHECK_EQ(lo.find("e")->as_str(), std::string("\xF0\x9F\x98\x8A"));
}

// json.cpp:125 / 127-128 — append_utf8 的 1 字节与 2 字节分支
//（3 字节既有 \u4e2d 已覆盖，4 字节由上面的代理对覆盖）。
PB_TEST(cov_core_json_short_utf8_units) {
  PB_CHECK_EQ(pb::json::parse("\"\\u0041\"").as_str(), std::string("A"));
  PB_CHECK_EQ(pb::json::parse("\"\\u00e9\"").as_str(), std::string("\xC3\xA9"));
}

// ============================ log ============================

// log.cpp:23 — level_name 对越界级别的兜底 "?"。LogLevel 底层类型固定为 int
//（log.h:8），static_cast 任意 int 值合法；开关覆盖全部枚举值故 99 落到兜底。
PB_TEST(cov_core_log_unknown_level_name) {
  std::string captured;
  pb::log_set_sink([&](pb::LogLevel, std::string_view line) { captured.assign(line); });
  pb::log_set_level(pb::LogLevel::Debug);
  pb::log_write(static_cast<pb::LogLevel>(99), "mystery");
  pb::log_set_sink(nullptr);  // 还原默认 sink / 级别，避免污染其他测试
  pb::log_set_level(pb::LogLevel::Info);
  PB_CHECK(captured.find("[?]") != std::string::npos);
}

// ===================== 追加：bt_radio / env_check 剩余未覆盖行 =====================

#include <algorithm>  // std::find

#include "core/bt_radio.h"

// bt_radio.cpp:72-79 — set_radios_enabled 的「无物理 radio」安全分支：返回 0 且
// err = "no physical bluetooth radio found"。仅当本机没有任何在场的物理蓝牙
// radio 时才真正调用：一旦存在物理 radio，set_radios_enabled 会对真实硬件下发
// DICS_ENABLE/DISABLE 类安装器调用（触碰物理设备），单测必须跳过。顺带断言
// list_radios 的 physical 过滤契约（逐条与 is_physical_radio_id 吻合）。
PB_TEST(cov_core_bt_radio_set_enabled_without_physical) {
  auto all = pb::btradio::list_radios();
  bool any_physical = false;
  for (auto& r : all) {
    PB_CHECK_EQ(r.physical, pb::btradio::is_physical_radio_id(r.instance_id));
    if (r.physical) any_physical = true;
  }
  if (any_physical) return;  // 保护真实硬件：绝不从单测改变设备状态
  std::string err = "untouched";
  int changed = pb::btradio::set_radios_enabled(false, &err);
  PB_CHECK_EQ(changed, 0);
  PB_CHECK_EQ(err, std::string("no physical bluetooth radio found"));
}

// env_check.cpp:201 / 232-233 — builtin_bluetooth_present 外层枚举耗尽 break 与
// 末尾 return false。本机蓝牙设备会使函数在 :229 提前 return true；把 our_vid
// 换成「每个在场 USB 厂商自己的 VID」再探测，等于把该厂商的键从候选集剔除——
// 若本机蓝牙检测恰好只依赖该厂商的设备，函数就会走完整个枚举（201 break）并
// 落到 232-233 的 return false。附带不变量：同一 our_vid 两次探测结果一致（纯
// 只读注册表）；剔除只会让检测结果从 true 收缩到 false。单调性断言仅在我们的
// 虚拟适配器键（VID_0B00）不在场时有效——它在场时换 our_vid 反而会把虚拟适配器
// 算作「外来蓝牙」，破坏集合包含关系，故此时跳过该断言。
PB_TEST(cov_core_env_bt_probe_vid_exclusion) {
  std::vector<std::string> vids;
  bool have_our_virtual_key = false;
  HKEY usb = nullptr;
  LONG rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0,
                          KEY_READ, &usb);
  PB_CHECK_EQ(rc, (LONG)ERROR_SUCCESS);
  if (rc == ERROR_SUCCESS) {
    for (DWORD i = 0;; ++i) {
      char sub[256];
      DWORD len = sizeof(sub);
      if (RegEnumKeyExA(usb, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        break;
      std::string k(sub, len);
      if (k.rfind("VID_", 0) != 0) continue;
      size_t amp = k.find('&');
      if (amp == std::string::npos) continue;
      std::string vid = k.substr(0, amp);
      if (vid == "VID_0B00") {
        have_our_virtual_key = true;
        continue;
      }
      if (std::find(vids.begin(), vids.end(), vid) == vids.end()) vids.push_back(vid);
    }
    RegCloseKey(usb);
  }
  PB_CHECK(!vids.empty());  // 本机存在 USB 设备（物理蓝牙 radio 即 VID_xxxx 键）
  bool base = pb::env::builtin_bluetooth_present("VID_0B00");
  PB_CHECK_EQ(pb::env::builtin_bluetooth_present("VID_0B00"), base);  // 只读 → 确定性
  for (auto& v : vids) {
    bool r = pb::env::builtin_bluetooth_present(v);
    if (!have_our_virtual_key) PB_CHECK(!r || base);  // 剔除只可能 true→false
  }
}

// ============ 追加（第 2 轮）：env_check.cpp:357-360 / 391，net.cpp:131-133 ============

#include <stdlib.h>  // __argc / __argv（MSVC CRT：识别被显式派生的子进程）
#include <string.h>  // strcmp

#include <aclapi.h>  // GetNamedSecurityInfo / SetEntriesInAcl / SetNamedSecurityInfo

// env_check.cpp:357-360 —「控制端口 3240 被其他程序占用」Warn 分支的「占位子进程」
// 本体。port_listened_by_other 按拥有者 PID 排除本进程（env_check.cpp:252），该分支
// 只能由另一个进程持有 3240 触发。仅当本 exe 以本测试名为过滤参数被显式派生时
// （见下个测试的 CreateProcessW），才以 SO_EXCLUSIVEADDRUSE 独占 3240，向继承的
// stdout 管道回写单字节就绪信号（内核保证 listen 生效先于字节写出，构成
// happens-before，无轮询无 sleep），随后挂起等待父进程终止。正常套件运行（无过滤
// 参数或过滤参数不是本测试名）时为无操作。
PB_TEST(cov_core_env_port3240_holder_child) {
  // 必须用窄字符 __argv/strcmp：tests/main.cpp 入口是窄字符 main(int, char**)，
  // 此类程序里 MSVC CRT 的全局 __wargv 恒为 NULL（仅 wmain 入口才填充）——读
  // __wargv[1] 会立刻 0xC0000005，子进程死在 bind 之前、永远写不出 'R'。
  if (__argc < 2 || strcmp(__argv[1], "cov_core_env_port3240_holder_child") != 0)
    return;  // 不是被派生的子进程：无操作
  net::WsaSession wsa;  // 子进程内自行初始化 Winsock
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  BOOL excl = TRUE;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(3240);
  char sig = 'F';
  if (s != INVALID_SOCKET &&
      ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl)) == 0 &&
      ::bind(s, (sockaddr*)&a, sizeof(a)) == 0 && ::listen(s, 8) == 0)
    sig = 'R';
  DWORD n = 0;
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);  // 父测试传入的管道写端
  if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, &sig, 1, &n, nullptr);
  for (;;) Sleep(INFINITE);  // 持有端口直到被终止；监听 socket 由内核随进程回收
}

// env_check.cpp:357-360 — 派生上面的占位子进程独占 3240 后，run_checks 第 7 项必须
// 走 Warn 分支：:358 status、:359 detail，以及 :360（then 体末尾越过 else 的跳转，
// 只有 then 体真正执行过才会被记录）。就绪信号是子进程回写的单字节——阻塞
// ReadFile，无时序假设；若本机 3240 本就被外部进程占用（pre_held），同样验证
// Warn。结束后终止子进程并确认端口释放（WaitForSingleObject 在进程句柄触发前
// 保证其全部句柄已被内核回收，故监听表确定性清空）。
PB_TEST(cov_core_env_run_checks_port_warn_external) {
  auto port_item = [](const std::vector<env::CheckResult>& cs) -> const env::CheckResult* {
    for (auto& c : cs)
      if (c.id == "port") return &c;
    return nullptr;
  };
  const bool pre_held = env::port_listened_by_other(3240);
  {  // 基线：第 7 项状态与监听表一致（空闲 → Ok，被外部占用 → Warn）
    auto cs = env::run_checks();
    const env::CheckResult* p = port_item(cs);
    PB_CHECK(p != nullptr);
    if (p)
      PB_CHECK_EQ((int)p->status, (int)(pre_held ? env::Status::Warn : env::Status::Ok));
  }

  // —— 派生占位子进程；stdout 重定向到管道，回写字节即就绪/失败信号 ——
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  PB_CHECK(CreatePipe(&rd, &wr, &sa, 0) != FALSE);
  PROCESS_INFORMATION pi{};
  bool spawned = false;
  if (rd && wr) {
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // 读端只留在父进程
    wchar_t exe[1024] = {};
    DWORD mfn = GetModuleFileNameW(nullptr, exe, 1024);
    if (mfn > 0 && mfn < 1024) {
      std::wstring cmd = std::wstring(L"\"") + exe + L"\" cov_core_env_port3240_holder_child";
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      // 子进程不读 stdin，必须传 nullptr：bInheritHandles=TRUE 时 STARTF_USESTDHANDLES
      // 里出现不可继承句柄（门禁/world.run 下父进程的 stdin 是非继承管道）会让
      // CreateProcessW 以 ERROR_INVALID_HANDLE 直接失败（spawned=false → held 失败）。
      // 与 test_runner.cpp run_capture 的既有写法一致（该路径在同门禁下已验证通过）。
      si.hStdInput = nullptr;
      si.hStdOutput = wr;
      si.hStdError = wr;
      spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &si, &pi) != FALSE;
    }
  }
  if (wr) CloseHandle(wr);  // 关掉父端写句柄：子进程若提前退出，ReadFile 得到 EOF 而非永久阻塞
  char sig = 0;
  bool got = false;
  if (spawned) {
    DWORD n = 0;
    got = ReadFile(rd, &sig, 1, &n, nullptr) != FALSE && n == 1;
  }
  const bool held = pre_held || (got && sig == 'R');  // 外部占用 或 子进程独占成功
  PB_CHECK(held);

  if (held) {
    PB_CHECK(env::port_listened_by_other(3240));  // 拥有者 PID != 本进程 → true
    auto cs = env::run_checks();
    const env::CheckResult* p = port_item(cs);
    PB_CHECK(p != nullptr);
    if (p) {
      PB_CHECK_EQ((int)p->status, (int)env::Status::Warn);  // :358
      PB_CHECK(p->detail.find(std::string("端口已被其他程序占用")) !=
               std::string::npos);  // :359（该文案只由本分支产生）
    }
  }

  // —— 清理：终止子进程，确认机器状态还原 ——
  if (spawned) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (rd) CloseHandle(rd);
  if (held && !pre_held) PB_CHECK(!env::port_listened_by_other(3240));
}

namespace {
// 当前进程令牌的用户 SID（LocalAlloc 拷贝，调用方负责 LocalFree）。
PSID current_user_sid() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return nullptr;
  DWORD need = 0;
  GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
  std::vector<BYTE> buf(need ? need : 1);
  PSID out = nullptr;
  if (GetTokenInformation(tok, TokenUser, buf.data(), need, &need)) {
    auto* tu = (TOKEN_USER*)buf.data();
    DWORD slen = GetLengthSid(tu->User.Sid);
    out = (PSID)LocalAlloc(LMEM_FIXED, slen);
    if (out && !CopySid(slen, out, tu->User.Sid)) {
      LocalFree(out);
      out = nullptr;
    }
  }
  CloseHandle(tok);
  return out;
}
}  // namespace

// env_check.cpp:391 — mark_first_run 在 RegCreateKeyExA(KEY_WRITE) 被拒时的提前
// 返回。对 HKCU\Software\PhantomBT 注入 deny-self-KEY_WRITE 的 DACL：deny 掩码恰为
// :389 请求的 KEY_WRITE，且不含 DELETE/WRITE_DAC（用户对自己键持有隐式 WRITE_DAC）
// ——完全可恢复的标准错误注入。断言链：①与 :389 相同的打开以 ERROR_ACCESS_DENIED
// 被拒（毒性生效）；②mark_first_run 后 FirstRunDone 仍缺失（:391 return，:393 未
// 执行；若 :389 意外成功则值会变 1，断言失败）。结束后按字节还原 DACL 与
// FirstRunDone，键若为本测试新建则整键删除。
PB_TEST(cov_core_env_mark_first_run_write_denied) {
  const char* kPath = "Software\\PhantomBT";
  const char* kNamed = "CURRENT_USER\\Software\\PhantomBT";  // 命名安全 API 的 HKCU 根
  const bool orig = env::first_run_done();

  bool key_existed = false;
  {
    HKEY probe = nullptr;
    key_existed = RegOpenKeyExA(HKEY_CURRENT_USER, kPath, 0, KEY_READ, &probe) == ERROR_SUCCESS;
    if (probe) RegCloseKey(probe);
  }

  HKEY base = nullptr;
  LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, kPath, 0, nullptr, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &base, nullptr);
  PB_CHECK_EQ(rc, (LONG)ERROR_SUCCESS);
  if (rc != ERROR_SUCCESS) return;

  // 备份 FirstRunDone 并使其缺失，让 :391 的效果可观察
  RegValueBlob fr{};
  bool had_fr = false;
  {
    DWORD type = 0, len = 0;
    if (RegQueryValueExA(base, "FirstRunDone", nullptr, &type, nullptr, &len) == ERROR_SUCCESS) {
      std::vector<BYTE> data(len ? len : 1);
      if (RegQueryValueExA(base, "FirstRunDone", nullptr, &type, data.data(), &len) ==
          ERROR_SUCCESS) {
        fr.name = "FirstRunDone";
        fr.type = type;
        fr.data.assign(data.begin(), data.begin() + len);
        had_fr = true;
      }
    }
    RegDeleteValueA(base, "FirstRunDone");
    RegCloseKey(base);
  }

  // —— 备份 DACL 并注入 deny-self-KEY_WRITE ——
  PSECURITY_DESCRIPTOR sd = nullptr;
  PACL old_dacl = nullptr;
  PSID sid = current_user_sid();
  PACL poisoned = nullptr;
  PB_CHECK(sid != nullptr);
  PB_CHECK_EQ(GetNamedSecurityInfoA((LPSTR)kNamed, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, &old_dacl, nullptr, &sd),
              (DWORD)ERROR_SUCCESS);
  if (sid) {
    EXPLICIT_ACCESS_A ea{};
    ea.grfAccessPermissions = KEY_WRITE;  // 恰是 mark_first_run(:389) 请求的掩码
    ea.grfAccessMode = DENY_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    TRUSTEE_A tr{};
    tr.TrusteeForm = TRUSTEE_IS_SID;
    tr.ptstrName = (LPSTR)sid;
    ea.Trustee = tr;
    PB_CHECK_EQ(SetEntriesInAclA(1, &ea, old_dacl, &poisoned), (DWORD)ERROR_SUCCESS);
  }
  if (poisoned) {
    DWORD sns = SetNamedSecurityInfoA((LPSTR)kNamed, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, poisoned, nullptr);
    PB_CHECK_EQ(sns, (DWORD)ERROR_SUCCESS);
    if (sns == ERROR_SUCCESS) {
      HKEY probe = nullptr;  // 毒性证明：与 :389 完全相同的 KEY_WRITE 打开必然被拒
      PB_CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, kPath, 0, KEY_WRITE, &probe),
                  (LONG)ERROR_ACCESS_DENIED);
      env::mark_first_run();         // :389 失败 → :391 提前返回
      PB_CHECK(!env::first_run_done());  // 值未被写入（成功路径 :393 会写入 1）
      DWORD rs = SetNamedSecurityInfoA((LPSTR)kNamed, SE_REGISTRY_KEY,
                                       DACL_SECURITY_INFORMATION, nullptr, nullptr, old_dacl,
                                       nullptr);
      PB_CHECK_EQ(rs, (DWORD)ERROR_SUCCESS);     // 属主隐式 WRITE_DAC，deny 不影响还原
      PB_CHECK(!env::first_run_done());  // 还原 DACL 后值仍未被写入
    }
  }

  // —— 精确还原 FirstRunDone 与键的存在性 ——
  if (had_fr) {
    HKEY kk = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &kk,
                        nullptr) == ERROR_SUCCESS) {
      RegSetValueExA(kk, fr.name.c_str(), 0, fr.type, fr.data.data(), (DWORD)fr.data.size());
      RegCloseKey(kk);
    }
  } else if (!key_existed) {
    RegDeleteTreeA(HKEY_CURRENT_USER, kPath);  // 键为本测试新建 → 整键移除
  }
  if (sid) LocalFree(sid);
  if (poisoned) LocalFree(poisoned);
  if (sd) LocalFree(sd);
  PB_CHECK_EQ(env::first_run_done(), orig);  // 机器状态完全还原
}

// net.cpp:131-133 — TcpListener::listen 的 socket() 失败分支。listen 不经
// getaddrinfo 直接 socket()；把进程的 WSAStartup 引用计数排干（循环 WSACleanup
// 直到失败——计数为 0 时它以 WSAENOTINITIALISED 失败，循环自带终止条件），此后
// socket() 必返回 INVALID_SOCKET 且 WSAGetLastError=10093，命中 :132 的错误文案与
// :133 的 return false。随后立即 WSAStartup 恢复并验证可用，不影响后续测试。
// 套件内测试按注册顺序串行执行、此处无并发线程，进程内确定性成立。
PB_TEST(cov_core_net_listen_socket_wsa_not_initialised) {
  int drained = 0;
  while (WSACleanup() == 0 && ++drained < 64) {
  }  // 之前各测试的 WsaSession 均已析构（计数平衡）；最多排 64 层防御
  std::string err = "untouched";
  net::TcpListener l;
  PB_CHECK(!l.listen(0, &err));  // :130 socket() 失败 → :131 → :132-133
  PB_CHECK_EQ(err, std::string("socket failed, WSAGetLastError=10093"));
  WSADATA d;
  PB_CHECK_EQ(WSAStartup(MAKEWORD(2, 2), &d), 0);  // 恢复进程 Winsock
  net::TcpListener probe;
  std::string e2;
  PB_CHECK(probe.listen(0, &e2));  // 恢复验证：Winsock 重新可用
}

// =========== 追加（第 3 轮）：env_check.cpp:322-323 管理员 Warn 分支 ===========

// env_check.cpp:322-323 —— 「管理员权限」未提升 Warn 分支。is_admin 的
// CheckTokenMembership(nullptr, ...)（env_check.cpp:111）在调用线程处于模拟
// （impersonation）状态时检查的是线程令牌而非进程令牌：用 CreateRestrictedToken
// 把进程令牌副本中的 BUILTIN\Administrators 组 SID 置为 deny-only（deny-only 的
// SID 在 CheckTokenMembership 语义里恒为「未启用」；进程未提升时该 SID 本就不在
// 令牌中，构造同样成功，两种提权上下文下测试均成立），SetThreadToken 挂到当前
// 线程后，同一进程内 is_admin() 即确定性返回 false，run_checks 第 4 项走 :322
// status=Warn / :323 detail。纯内存令牌副本：不派生子进程、不触碰机器状态，
// 离开作用域即还原线程令牌（先备份、后精确还原，含异常路径）。
PB_TEST(cov_core_env_admin_warn_restricted_token) {
  const bool orig_admin = env::is_admin();  // 记录进入前状态，结尾断言还原

  // —— 构造 deny-only 管理员 SID 的模拟令牌副本 ——
  SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
  PSID admins = nullptr;
  PB_CHECK(AllocateAndInitializeSid(&nt_auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                    DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                    &admins) != FALSE);
  HANDLE proc_tok = nullptr, restr = nullptr;
  PB_CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                            &proc_tok) != FALSE);
  if (proc_tok) {
    HANDLE imp = nullptr;
    // TOKEN_IMPERSONATE 必须随副本授予：CreateRestrictedToken 产出的句柄沿袭
    // 副本的授权集，缺它则 SetThreadToken 以 ERROR_ACCESS_DENIED(5) 拒绝（实测）。
    PB_CHECK(DuplicateTokenEx(proc_tok, TOKEN_DUPLICATE | TOKEN_IMPERSONATE, nullptr,
                              SecurityImpersonation, TokenImpersonation, &imp) != FALSE);
    if (imp) {
      SID_AND_ATTRIBUTES deny[1] = {{admins, 0}};
      PB_CHECK(CreateRestrictedToken(imp, 0, 1, deny, 0, nullptr, 0, nullptr,
                                     &restr) != FALSE);
      CloseHandle(imp);
    }
  }
  PB_CHECK(restr != nullptr);

  // —— 备份当前线程令牌（正常情况没有），挂上受限令牌；作用域结束无条件还原 ——
  HANDLE prev_tok = nullptr;
  const bool had_prev =
      OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &prev_tok) != FALSE;
  const bool ok = restr && SetThreadToken(nullptr, restr) != FALSE;
  PB_CHECK(ok);
  if (ok) {
    struct RestoreTok {
      HANDLE prev;
      bool had;
      ~RestoreTok() {
        if (had) {
          SetThreadToken(nullptr, prev);
          CloseHandle(prev);
        } else {
          SetThreadToken(nullptr, nullptr);  // 进入前线程无模拟令牌 → 清除即还原
        }
      }
    } restore{prev_tok, had_prev};  // 提前 return/异常均会析构，线程令牌绝不泄漏
    PB_CHECK(!env::is_admin());  // deny-only 管理员 SID → 非管理员（自验 deny 语义）
    auto checks = env::run_checks();  // 第 4 项在本线程令牌上评估 → :322-323
    const env::CheckResult* a = nullptr;
    for (auto& c : checks)
      if (c.id == "admin") a = &c;
    PB_CHECK(a != nullptr);
    if (a) {
      PB_CHECK_EQ((int)a->status, (int)env::Status::Warn);                    // :322
      PB_CHECK(a->detail.find(std::string("普通权限")) != std::string::npos);  // :323
    }
  }
  if (!ok && had_prev) CloseHandle(prev_tok);  // 未挂上则线程令牌未变，只回收备份句柄
  if (restr) CloseHandle(restr);
  if (proc_tok) CloseHandle(proc_tok);
  if (admins) FreeSid(admins);
  PB_CHECK_EQ(env::is_admin(), orig_admin);  // 线程令牌已还原，与进入前一致
}

// ====== 追加（第 1 轮任务B）：bt_radio 枚举形状 / 虚拟设备排除 / sys-bt 行格式 ======

#include "control/commands.h"
#include "sim/simulator.h"

// bt_radio.cpp:32-35 — is_physical_radio_id 的 4 字符边界：compare(0,4,...) 要求
// size>=4 且前缀恰为 "USB\"/"usb\"。恰 4 字符的 "USB\" 是最小合法输入（true），
// 3 字符 "USB" 被 <4 守卫挡下；第 4 字符不是分隔符（"USBF..."）同样 false。
// bt_radio.cpp:24-29 — is_our_virtual 大小写不敏感子串匹配只认 "VID_0B00"：
// 他商 VID、或 PID 位上的 0B00，都不得误判（find 找的是完整 "VID_0B00" 串）。
PB_TEST(cov_core_bt_radio_predicates_boundary) {
  PB_CHECK(pb::btradio::is_physical_radio_id("USB\\VID_8087&PID_0026\\5&1B50F57F&0&14"));
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("USB\\"), true);   // 恰 4 字符：最小命中
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("usb\\"), true);   // 小写前缀同样命中
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("USB"), false);    // 3 字符：<4 守卫
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("usb"), false);
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id(""), false);
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("USBF\\VID_1&2\\3"), false);  // 第 4 字符非 '\'
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("BTHLEDEVICE\\{G}\\8&1&F"), false);
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("root\\phantom\\0"), false);
  // 虚拟适配器：混合大小写命中；他商 VID（0B05）与 PID 位 0B00 不命中。
  PB_CHECK(pb::btradio::is_our_virtual("UsB\\vId_0b00&pId_0001\\5&0"));
  PB_CHECK(!pb::btradio::is_our_virtual("USB\\VID_1234&PID_0B00&REV_0001\\x"));  // PID 非 VID
  PB_CHECK(!pb::btradio::is_our_virtual("USB\\VID_0B05&PID_0011\\x"));           // 近邻厂商
  PB_CHECK(!pb::btradio::is_our_virtual(""));
}

// bt_radio.cpp:37-70 — list_radios 枚举形状的机器级不变量（只读，非管理员合法）：
// 1) 连续两次枚举完全一致（instance_id/physical/disabled 逐一相等）——SetupDi
//    快照在同一进程相邻两次调用间是确定性的；
// 2) instance_id 无重复（同一 DevInst 不会被枚举成两行）；
// 3) 每条 id 非空且 < MAX_DEVICE_ID_LEN（cfgmgr32.h 定为 200，正是
//    SetupDiGetDeviceInstanceIdA 输出缓冲的约束）；
// 4) physical 标志与 is_physical_radio_id 逐条吻合——本测试让该契约无条件成立
//    （cov_core_bt_radio_set_enabled_without_physical 里的同款检查在其函数内部，
//    本测试独立于「本机是否有物理 radio」）；
// 5) 结果绝不含我们的虚拟适配器（VID_0B00 排除，bt_radio.cpp:48）。
PB_TEST(cov_core_bt_radio_list_shape_deterministic) {
  auto a = pb::btradio::list_radios();
  auto b = pb::btradio::list_radios();
  PB_CHECK_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    PB_CHECK_EQ(a[i].instance_id, b[i].instance_id);  // 1) 确定性
    PB_CHECK_EQ(a[i].physical, b[i].physical);
    PB_CHECK_EQ(a[i].disabled, b[i].disabled);
  }
  for (size_t i = 0; i < a.size(); ++i) {
    PB_CHECK(!a[i].instance_id.empty());                  // 3) 非空
    PB_CHECK(a[i].instance_id.size() < 200u);             // 3) MAX_DEVICE_ID_LEN
    PB_CHECK_EQ(a[i].physical,
                pb::btradio::is_physical_radio_id(a[i].instance_id));  // 4)
    PB_CHECK(!pb::btradio::is_our_virtual(a[i].instance_id));         // 5)
    for (size_t j = i + 1; j < a.size(); ++j)             // 2) 无重复
      PB_CHECK(a[i].instance_id != a[j].instance_id);
  }
}

// sys-bt list 的行格式契约（simulator.cpp:284-296 → commands.cpp:50-57）：帧为
// {"OK", 行..., "OK-END"}；空表 → 单行 "no physical bluetooth radio found"；
// 非空 → 每行 "[disabled] "/"[enabled]  " + "RADIO "/"child  " + 描述 +
// " (" + instance_id + ")"，并与紧邻的 list_radios() 快照逐行对齐（条数、
// 禁用/物理标记、行尾 "(instance_id)"）。两次枚举间若热插拔（快照不一致）则
// 退化为仅格式断言。复用 test_global_settings.cpp:14-18 的模式：Simulator 不
// start（纯模型层），cmd_sys_bt 只读 SetupDi，绝不改变设备状态。
PB_TEST(cov_core_bt_radio_sys_bt_list_line_format) {
  sim::Simulator sim;  // 未 start：cmd_sys_bt 不依赖 usbip 服务器
  auto before = pb::btradio::list_radios();
  auto r = control::execute(sim, "sys-bt list");
  PB_CHECK(r.size() >= 2u);
  if (r.size() < 2u) return;
  PB_CHECK_EQ(r.front(), std::string("OK"));
  PB_CHECK_EQ(r.back(), std::string("OK-END"));
  size_t body_n = r.size() - 2;
  if (body_n == 1 && r[1] == "no physical bluetooth radio found") {
    PB_CHECK(before.empty());  // 空表文案与真实枚举一致
    return;
  }
  for (size_t i = 0; i < body_n; ++i) {
    const std::string& line = r[i + 1];
    PB_CHECK(line.rfind("[disabled] ", 0) == 0 || line.rfind("[enabled]  ", 0) == 0);
    PB_CHECK(line.find("RADIO ") != std::string::npos ||
             line.find("child  ") != std::string::npos);
    PB_CHECK(!line.empty() && line.back() == ')');
  }
  auto after = pb::btradio::list_radios();
  if (after.size() != before.size()) return;  // 热插拔竞态：跳过逐行对齐
  bool aligned = true;
  for (size_t i = 0; i < before.size(); ++i)
    if (before[i].instance_id != after[i].instance_id) aligned = false;
  if (!aligned) return;
  PB_CHECK_EQ(body_n, after.size());  // 行数与 RadioDevice 条数一致
  for (size_t i = 0; i < after.size() && i < body_n; ++i) {
    const std::string& line = r[i + 1];
    const char* state = after[i].disabled ? "[disabled] " : "[enabled]  ";
    const char* kind = after[i].physical ? "RADIO " : "child  ";
    PB_CHECK(line.rfind(state, 0) == 0);
    PB_CHECK(line.find(kind) != std::string::npos);
    std::string suffix = " (" + after[i].instance_id + ")";
    PB_CHECK(line.size() >= suffix.size());
    if (line.size() >= suffix.size())
      PB_CHECK(line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0);
  }
}

// ===== 追加（第 2 轮任务B）：set_radios_enabled 的未提权拒绝 / 无电台实路径 =====
// 盘点矩阵行「物理电台枚举/禁用/启用（core/bt_radio，sys-bt 底层）」中仍缺的
// 状态切换轴：真实 disable/enable 翻转需管理员（tools/check-tools.ps1 H3
// sysbt-real-toggle-roundtrip 已在提权 shell 下自动化），但未提权的「被拒」与
// 无电台的「提前返回」两条确定性实路径此前没有任何断言。安全性依据：设备
// 启用/禁用是管理员专属的 PnP 策略（bt_radio.h:4-5 契约）——未提权进程的
// SetupDiCallClassInstaller(DICS_*) 必被拒且状态零改动；2026-11-01 本机以
// runas /trustlevel:0x20000 的非提权进程实测：disable/enable 双向均回
// "ERR failed: USB\VID_8087&PID_0026\...: error 5;"，前后 [disabled]/[enabled]
// 快照逐行一致（tools/check-tools.ps1 F2 同款代码 PASS，见本轮 notes）。

// bt_radio.cpp:89-125 — set_radios_enabled 的「类安装器拒绝」实路径（未提权真实
// 调用）：覆盖 89-121 主循环（按 instance_id 找回 DevInfo、DICS_ENABLE/DISABLE
// 参数组装、安装器调用失败与 failures 累积 :116-119）与 123-125 的 changed==0
// 报错分支。提权运行则 return（那会真实翻转硬件状态，属 H3 的受控领域）；无
// 物理电台的空表分支（:77-80）由 cov_core_bt_radio_set_enabled_without_physical
// 覆盖。若状态竟被改动（OS 策略被打破的兜底），先按基线尽力还原再让断言失败。
PB_TEST(cov_core_bt_radio_set_enabled_denied_unelevated) {
  if (env::is_admin()) return;  // 提权调用会真实翻转硬件状态：绝不从单测触发
  auto before = pb::btradio::list_radios();
  bool any_physical = false;
  for (auto& r : before) any_physical = any_physical || r.physical;
  if (!any_physical) return;  // 空表分支见 cov_core_bt_radio_set_enabled_without_physical

  std::string err;
  PB_CHECK_EQ(pb::btradio::set_radios_enabled(false, &err), 0);  // disable 被拒
  PB_CHECK_EQ(pb::btradio::set_radios_enabled(true, &err), 0);   // enable 同样被拒
  PB_CHECK(err.rfind("failed: ", 0) == 0);                // :124 "failed: " + failures
  PB_CHECK(err.find(": error ") != std::string::npos);    // :118-119 ": error N; "

  auto after = pb::btradio::list_radios();
  bool clean = after.size() == before.size();  // 兜底判定：状态是否零改动
  for (size_t i = 0; clean && i < after.size(); ++i)
    clean = after[i].instance_id == before[i].instance_id && after[i].disabled == before[i].disabled;
  if (!clean) {  // OS 策略被打破（理论不可能）：按首个物理电台的基线状态尽力还原
    bool base_enabled = true;
    for (auto& r : before)
      if (r.physical) base_enabled = !r.disabled;
    std::string restore_err;
    (void)pb::btradio::set_radios_enabled(base_enabled, &restore_err);
  }
  PB_CHECK_EQ(after.size(), before.size());
  for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
    PB_CHECK_EQ(after[i].instance_id, before[i].instance_id);
    if (after[i].instance_id == before[i].instance_id)
      PB_CHECK_EQ(after[i].disabled, before[i].disabled);  // 未提权 → 零状态改动
  }
}

// 命令层契约（commands.cpp:50-57/97-99 + simulator.cpp:322-329）：sys-bt
// disable/enable 在两条确定性实路径上的 ERR 帧形状——
//   1) 无物理电台（任意提权状态均可测，:77-80 在任何 SetupDi 调用前提前返回）：
//      单帧 "ERR no physical bluetooth radio found"（bt_radio.cpp:78 的 err 经
//      simulator.cpp:327 抛出、commands.cpp:98 成帧）；
//   2) 有物理电台 + 未提权：单帧 "ERR failed: <instance_id>: error N"。
// 提权 + 有电台时 return（真实翻转，check-tools.ps1 H3 的领域）。Simulator 不
// start——cmd_sys_bt 不依赖 usbip 服务器（与 cov_core_bt_radio_sys_bt_list_line_format
// 同模式）；两条路径均零状态改动（未提权被 OS 拒 / 空表提前返回），结尾断言。
PB_TEST(cov_core_bt_radio_sysbt_disable_enable_err_paths) {
  sim::Simulator sim;
  auto before = pb::btradio::list_radios();
  bool any_physical = false;
  for (auto& r : before) any_physical = any_physical || r.physical;
  if (any_physical && env::is_admin()) return;  // 提权真实翻转：H3 的受控领域

  for (const char* op : {"disable", "enable"}) {
    auto r = control::execute(sim, std::string("sys-bt ") + op);
    PB_CHECK_EQ(r.size(), 1u);  // ERR 路径：commands.cpp:98 的单帧，无 OK/OK-END 包裹
    if (r.size() == 1) {
      if (any_physical) {
        PB_CHECK(r[0].rfind("ERR failed: ", 0) == 0);
        PB_CHECK(r[0].find(": error ") != std::string::npos);
      } else {
        PB_CHECK_EQ(r[0], std::string("ERR no physical bluetooth radio found"));
      }
    }
  }
  auto after = pb::btradio::list_radios();
  PB_CHECK_EQ(after.size(), before.size());
  for (size_t i = 0; i < before.size() && i < after.size(); ++i)
    if (after[i].instance_id == before[i].instance_id)
      PB_CHECK_EQ(after[i].disabled, before[i].disabled);  // 两条路径均零状态改动
}

// ====== 追加（第 3 轮任务A）：env_check.cpp:201/232-233 枚举耗尽路径 ======

namespace {

// 在 fake hive 的 Enum\USB 下造一个设备实例键：VID&PID 键 → 实例键 → 可选值。
// service/klass/compat 传 nullptr 表示不写该值；has_flags=false 表示不写
// ConfigFlags（env_check 的 instance_active 对缺失值按「在场」处理）。
void fake_usb_device(HKEY usb, const char* vidpid, const char* inst, const char* service,
                     const char* klass, const char* compat, DWORD config_flags, bool has_flags) {
  HKEY vk = nullptr;
  if (RegCreateKeyExA(usb, vidpid, 0, nullptr, 0, KEY_WRITE, nullptr, &vk, nullptr) != ERROR_SUCCESS)
    return;
  HKEY ik = nullptr;
  if (RegCreateKeyExA(vk, inst, 0, nullptr, 0, KEY_WRITE, nullptr, &ik, nullptr) == ERROR_SUCCESS) {
    if (service)
      RegSetValueExA(ik, "Service", 0, REG_SZ, (const BYTE*)service, (DWORD)strlen(service) + 1);
    if (klass)
      RegSetValueExA(ik, "Class", 0, REG_SZ, (const BYTE*)klass, (DWORD)strlen(klass) + 1);
    if (compat) {
      std::string mz(compat);
      mz += '\0';
      mz += '\0';  // REG_MULTI_SZ 要求双 NUL 终止
      RegSetValueExA(ik, "CompatibleIDs", 0, REG_MULTI_SZ, (const BYTE*)mz.data(), (DWORD)mz.size());
    }
    if (has_flags)
      RegSetValueExA(ik, "ConfigFlags", 0, REG_DWORD, (const BYTE*)&config_flags,
                     sizeof(config_flags));
    RegCloseKey(ik);
  }
  RegCloseKey(vk);
}

}  // namespace

// env_check.cpp:201 / 232-233 —— builtin_bluetooth_present 外层枚举耗尽后的
// break、收尾 RegCloseKey 与 return false。真机上无法确定性抵达：Enum\USB 里
// 同时存在多个不同厂商的蓝牙键（本机实测：Intel VID_8087、IMC VID_13D3、
// Realtek/MediaTek 组合设备），our_vid 一次只能剔除一个厂商，任何剔除组合都会
// 命中剩下的蓝牙键而在 :229 提前 return true——第 2 轮的 VID 剔除测试
// （cov_core_env_bt_probe_vid_exclusion）正因此没有覆盖这三行。改用
// RegOverridePredefKey（advapi32，微软文档语义：把当前进程的预定义 HKLM 重定向
// 到给定键、传 NULL 还原；两条语义均以独立 PowerShell P/Invoke 探针实测确认）
// 将本进程 HKLM 指向完全受控的 HKCU 测试 hive，使被测函数的输入与机器状态
// 解耦，确定性覆盖：
//   1) 只有非蓝牙外设 + 我们自己的 VID_0B00 键（带全套蓝牙标记，验证剔除）
//      → 枚举走到耗尽：201 break → 232 RegCloseKey → 233 return false；
//   2) 正向对照：加入 known_bt_vid 榜上的 CSR VID_0A12 键（无任何描述值）
//      → 仅凭厂商轴即判蓝牙 → return true，证明 fake 数据确被读取；
//   3) 给该实例写 ConfigFlags=CONFIGFLAG_DISABLED(0x1) → instance_active 为
//      false → 再次耗尽 → return false（env_check.cpp:181-189 的禁用语义）。
// 作用域守卫保证任何退出路径都先还原 HKLM 映射、再删测试键；结尾以真实 HKLM
// 独有的 SOFTWARE\Microsoft 分支自检还原成功（fake hive 里不存在该分支）。
PB_TEST(cov_core_env_builtin_bt_fake_hive_exhaustion) {
  const char* kHiveRel = "Software\\PhantomBT\\UnitTestHive";
  HKEY hive = nullptr;
  LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, kHiveRel, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                            nullptr, &hive, nullptr);
  PB_CHECK_EQ(rc, (LONG)ERROR_SUCCESS);
  if (rc != ERROR_SUCCESS) return;
  {
    struct RestoreMap {
      bool active = false;
      ~RestoreMap() {
        if (active) RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);  // 精确还原
      }
    } restore;
    HKEY usb = nullptr;
    rc = RegCreateKeyExA(hive, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, nullptr, 0, KEY_WRITE,
                         nullptr, &usb, nullptr);
    PB_CHECK_EQ(rc, (LONG)ERROR_SUCCESS);
    if (rc == ERROR_SUCCESS) {
      // 非蓝牙存储设备：Service/Class/CompatibleIDs 三轴全非蓝牙
      fake_usb_device(usb, "VID_1234&PID_5678", "5&2&0", "USBSTOR", "DiskDrive",
                      "USB\\Class_08&SubClass_06", 0, true);
      // 非 VID_ 前缀键：foreign_vid_enum_key 直接跳过（:203 continue）
      fake_usb_device(usb, "ROOT_HUB30", "5&1&0", "USBHUB3", "USB", nullptr, 0, false);
      // 我们自己的虚拟适配器键：带全套蓝牙标记，必须被 our_vid 剔除——
      // 剔除一旦失效，下面场景 1 的 !present 断言会立刻失败
      fake_usb_device(usb, "VID_0B00&PID_0001", "5&0&0", "BTHUSB", "Bluetooth",
                      "USB\\Class_E0&SubClass_01", 0, true);
      RegCloseKey(usb);
      restore.active = RegOverridePredefKey(HKEY_LOCAL_MACHINE, hive) == ERROR_SUCCESS;
      PB_CHECK(restore.active);
    }
    if (restore.active) {
      {  // 映射自检：经预定义 HKLM 必须读到 fake hive 里的值，否则负向断言无意义
        HKEY k = nullptr;
        PB_CHECK_EQ(RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                                  "SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_1234&PID_5678\\5&2&0",
                                  0, KEY_READ, &k),
                    (LONG)ERROR_SUCCESS);
        if (k) {
          char b[32] = {};
          DWORD n = sizeof(b) - 1;
          PB_CHECK_EQ(RegQueryValueExA(k, "Service", nullptr, nullptr, (LPBYTE)b, &n),
                      (LONG)ERROR_SUCCESS);
          PB_CHECK_EQ(std::string(b), std::string("USBSTOR"));
          RegCloseKey(k);
        }
      }
      // 1) 无外来蓝牙键 → 枚举耗尽：:201 break → :232 RegCloseKey → :233 return false
      PB_CHECK(!pb::env::builtin_bluetooth_present("VID_0B00"));
      // 2) 正向对照：CSR 厂商键、无任何描述值 → 仅凭厂商轴判定 → return true
      HKEY usb2 = nullptr;
      PB_CHECK_EQ(RegCreateKeyExA(hive, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, nullptr, 0,
                                  KEY_WRITE, nullptr, &usb2, nullptr),
                  (LONG)ERROR_SUCCESS);
      if (usb2) {
        fake_usb_device(usb2, "VID_0A12&PID_0001", "5&3&0", nullptr, nullptr, nullptr, 0, false);
        RegCloseKey(usb2);
      }
      PB_CHECK(pb::env::builtin_bluetooth_present("VID_0B00"));
      // 3) 禁用该实例 → 非在场 → 再次走完整个枚举 → return false
      HKEY ik = nullptr;
      PB_CHECK_EQ(RegOpenKeyExA(hive, "SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_0A12&PID_0001\\5&3&0",
                                0, KEY_SET_VALUE, &ik),
                  (LONG)ERROR_SUCCESS);
      if (ik) {
        DWORD disabled = 1;  // CONFIGFLAG_DISABLED（env_check.cpp:184）
        RegSetValueExA(ik, "ConfigFlags", 0, REG_DWORD, (const BYTE*)&disabled, sizeof(disabled));
        RegCloseKey(ik);
      }
      PB_CHECK(!pb::env::builtin_bluetooth_present("VID_0B00"));
    }
  }  // ← 守卫析构：先还原 HKLM 映射，测试键随后才被删除
  RegCloseKey(hive);
  PB_CHECK_EQ(RegDeleteTreeA(HKEY_CURRENT_USER, kHiveRel), (LONG)ERROR_SUCCESS);
  HKEY real = nullptr;  // 还原自检：真实 HKLM 独有的分支重新可见（hive 里没有）
  PB_CHECK_EQ(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft", 0, KEY_READ, &real),
              (LONG)ERROR_SUCCESS);
  if (real) RegCloseKey(real);
}

// ====== 追加（第 3 轮任务B）：虚拟适配器 id「物理形态」危害锚定 ======
// 盘点行「物理电台枚举/禁用/启用（core/bt_radio，sys-bt 底层）」三轴已各有断言
//（本文件上方各测试 + test_sim_control_cov.cpp
// cov_sim_control_sys_bt_real_toggle_roundtrip + check-tools.ps1 F/F2/H3）。仍缺的
// 一块：bt_radio.cpp:48 的 VID_0B00 排除是承重墙——我们的虚拟适配器实例 id 以
// "USB\" 开头，会通过 is_physical_radio_id（bt_radio.cpp:32-35 只看 USB\ 前缀）被
// 标 physical=true；且其设备/接口类码为 0xE0/01/01（adapter_model.cpp:63-67，
// Bluetooth 编程接口），内核挂载后落进 Bluetooth 设备类。若无 :48 的排除，
// list_radios 会把虚拟适配器当物理 RADIO 列出，sys-bt disable 会真的去禁用我们
// 自己的适配器。本测试在谓词层钉死该危害组合：
//   我们的 id ⇒ is_our_virtual==true 且 is_physical_radio_id==true（两种大小写形态）。
// 任一谓词被改动使该组合不再成立（如大小写处理收紧/放松）都先在这里报警。
// 「枚举确实包含 VID_0B00 设备时 :48 的 continue 真正触发」的活体路径需把虚拟
// 适配器挂进内核（usbip attach + 管理员），属真机腿：由 tools/check-tools.ps1
// H5 sysbt-list-excludes-live-virtual-adapter 自动化（本机提权门禁下执行）。
PB_TEST(cov_core_bt_radio_virtual_id_is_physical_shaped) {
  const char* kOurIds[] = {
      "USB\\VID_0B00&PID_0001\\5&1B50F57F&0&10",
      "usb\\vid_0b00&pid_0001\\5&2ab8f6d&0&10",
  };
  for (auto* id : kOurIds) {
    PB_CHECK_EQ(pb::btradio::is_our_virtual(id), true);        // 是我们的虚拟适配器
    PB_CHECK_EQ(pb::btradio::is_physical_radio_id(id), true);  // 且长得像物理电台
  }
  // 对照：真实物理电台（Intel）同样「物理形态」，但不是我们的——USB\ 前缀无法
  // 区分我们与外来电台，排除只能靠 VID_0B00（这正是 :48 存在的理由）。
  PB_CHECK_EQ(pb::btradio::is_physical_radio_id("USB\\VID_8087&PID_0026\\5&1B50F57F&0&14"), true);
  PB_CHECK(!pb::btradio::is_our_virtual("USB\\VID_8087&PID_0026\\5&1B50F57F&0&14"));
  // BTHENUM 子设备既非物理形态也非我们的（双重保障：不会被 set_radios_enabled
  // 触碰，也不会被 :48 误伤）。
  PB_CHECK(!pb::btradio::is_physical_radio_id("BTHENUM\\DEV_B88411768C8A\\7&34FA533"));
}
