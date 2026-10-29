// phantombt-gui.exe：Win32 GUI（仪器面板风：左导航 + 深色主题，见 theme.h）
// 页面：设备（加载/attach/detach/注入）、BLE（广播/键入）、日志（实时）、自动测试
// 全宽字符 API（/utf-8 下中文 UI 必须走 W 系列）
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

#include "ble/ble_device.h"
#include "control/commands.h"
#include "control/pipe_server.h"
#include "core/bytes.h"
#include "core/env_check.h"
#include "core/log.h"
#include "core/test_runner.h"
#include "gui/env_dialog.h"
#include "gui/theme.h"
#include "sim/simulator.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' "                \
                         "name='Microsoft.Windows.Common-Controls' "         \
                         "version='6.0.0.0' processorArchitecture='*' "      \
                         "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int WM_APP_LOG = WM_APP + 1;
constexpr int WM_APP_TESTLINE = WM_APP + 2;  // wp = char* (utf-8 line), UI frees
constexpr int WM_APP_TESTDONE = WM_APP + 3;  // wp = exit code (-1 launch fail)

enum CtrlId {
  IDC_NAV_DEVICE = 10,
  IDC_NAV_BLE = 11,
  IDC_NAV_LOG = 12,
  IDC_NAV_TEST = 13,
  IDC_BTN_LOAD_HID = 101,
  IDC_BTN_LOAD_BT = 102,
  IDC_BTN_ATTACH = 103,
  IDC_BTN_DETACH = 104,
  IDC_EDIT_INJECT = 105,
  IDC_BTN_INJECT = 106,
  IDC_BTN_BLE_ADV = 201,
  IDC_BTN_BLE_STOP = 202,
  IDC_EDIT_TYPE = 203,
  IDC_BTN_TYPE = 204,
  IDC_BTN_RUNTESTS = 301,
  IDC_BTN_ENVCHECK = 302,
};

pb::sim::Simulator g_sim;
pb::control::PipeServer* g_pipe = nullptr;
HWND g_main = nullptr;
HWND g_nav_dev = nullptr, g_nav_ble = nullptr, g_nav_log = nullptr, g_nav_test = nullptr;
HWND g_page_dev = nullptr, g_page_ble = nullptr, g_page_log = nullptr, g_page_test = nullptr;
HWND g_list = nullptr, g_inject_edit = nullptr, g_empty_lbl = nullptr;
HWND g_ble_list = nullptr, g_type_edit = nullptr, g_log = nullptr;
HWND g_test_out = nullptr, g_test_status = nullptr, g_test_btn = nullptr;
COLORREF g_test_status_color = 0;
std::thread g_test_thread;
HANDLE g_test_stop = nullptr;  // auto-reset event; set on shutdown to cancel the run
std::string g_test_output;     // worker writes, UI reads after WM_APP_TESTDONE
HBRUSH g_br_surface = nullptr, g_br_bg = nullptr;
int g_page = 0;

std::wstring to_w(const std::string& s) { return pb::to_w(s); }

void append_log_sync(const std::string& utf8) {
  if (!g_log) return;
  std::wstring w = to_w(utf8);
  int len = GetWindowTextLengthW(g_log);
  SendMessageW(g_log, EM_SETSEL, len, len);
  SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
  if (len > 400000) {
    SendMessageW(g_log, EM_SETSEL, 0, len / 2);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
  }
}

void run_and_show(const std::string& cmd) {
  auto replies = pb::control::execute(g_sim, cmd);
  std::string all;
  for (auto& r : replies) all += "> " + cmd + "\n" + r + "\n";
  append_log_sync(all);
}

void select_page(int idx) {
  g_page = idx;
  SendMessageW(g_nav_dev, pb::gui::PB_NAV_SELECT, idx == 0, 0);
  SendMessageW(g_nav_ble, pb::gui::PB_NAV_SELECT, idx == 1, 0);
  SendMessageW(g_nav_log, pb::gui::PB_NAV_SELECT, idx == 2, 0);
  SendMessageW(g_nav_test, pb::gui::PB_NAV_SELECT, idx == 3, 0);
  ShowWindow(g_page_dev, idx == 0 ? SW_SHOW : SW_HIDE);
  ShowWindow(g_page_ble, idx == 1 ? SW_SHOW : SW_HIDE);
  ShowWindow(g_page_log, idx == 2 ? SW_SHOW : SW_HIDE);
  ShowWindow(g_page_test, idx == 3 ? SW_SHOW : SW_HIDE);
  InvalidateRect(g_main, nullptr, TRUE);
}

// —— 自动测试页 ——

void test_append(const std::string& utf8) {
  if (!g_test_out) return;
  std::wstring w = to_w(utf8);
  w += L"\r\n";
  int len = GetWindowTextLengthW(g_test_out);
  SendMessageW(g_test_out, EM_SETSEL, len, len);
  SendMessageW(g_test_out, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
  if (len > 400000) {  // same cap as the log page
    SendMessageW(g_test_out, EM_SETSEL, 0, len / 2);
    SendMessageW(g_test_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
  }
  SendMessageW(g_test_out, EM_SCROLLCARET, 0, 0);
}

void test_set_status(const wchar_t* text, COLORREF color) {
  if (!g_test_status) return;
  g_test_status_color = color;
  SetWindowTextW(g_test_status, text);
  InvalidateRect(g_test_status, nullptr, TRUE);
}

void start_auto_tests() {
  if (g_test_thread.joinable()) return;  // a run is already in flight
  std::string exe = pb::testrunner::find_tests_exe();
  SetWindowTextW(g_test_out, L"");
  g_test_output.clear();
  if (exe.empty()) {
    test_append("未找到 phantombt-tests.exe —— 请先在仓库根目录运行 build.bat 生成测试二进制。");
    test_set_status(L"未找到测试二进制", pb::gui::AMBER);
    return;
  }
  EnableWindow(g_test_btn, FALSE);
  test_set_status(L"运行中…", pb::gui::TEXT_DIM);
  test_append("启动: " + exe);
  g_test_thread = std::thread([exe]() {
    g_test_output.clear();
    int rc = pb::testrunner::run_capture(
        "\"" + exe + "\"",
        [&](const std::string& line) {
          g_test_output += line;
          g_test_output += "\n";
          if (!IsWindow(g_main)) return;
          char* p = new char[line.size() + 1];
          memcpy(p, line.c_str(), line.size() + 1);
          if (!SendMessageW(g_main, WM_APP_TESTLINE, (WPARAM)p, 0)) delete[] p;
        },
        g_test_stop);
    // Post (async!): a synchronous SendMessage here deadlocks — the UI thread
    // handles WM_APP_TESTDONE by joining this very worker.
    if (IsWindow(g_main)) PostMessageW(g_main, WM_APP_TESTDONE, (WPARAM)rc, 0);
  });
}

void refresh_counts() {
  int hid = 0, ble = 0;
  for (auto& l : pb::control::execute(g_sim, "list"))
    if (l.rfind("DEV ", 0) == 0) ++hid;
  for (auto& l : pb::control::execute(g_sim, "ble-list"))
    if (!l.empty() && l.rfind("OK", 0) != 0) ++ble;
  SendMessageW(g_nav_dev, pb::gui::PB_NAV_BADGE, hid, 0);
  SendMessageW(g_nav_ble, pb::gui::PB_NAV_BADGE, ble, 0);
}

void refresh_device_list() {
  if (!g_list) return;
  SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
  auto lines = pb::control::execute(g_sim, "list");
  int row = 0;
  if (g_empty_lbl) {
    bool any = false;
    for (auto& l : lines)
      if (l.rfind("DEV ", 0) == 0) any = true;
    ShowWindow(g_empty_lbl, any ? SW_HIDE : SW_SHOW);
  }
  for (auto& l : lines) {
    if (l.rfind("DEV ", 0) != 0) continue;
    std::string rest = l.substr(4);
    size_t s1 = rest.find(' ');
    size_t s2 = rest.find(' ', s1 + 1);
    std::string busid = rest.substr(0, s1);
    std::string vidpid = rest.substr(s1 + 1, s2 - s1 - 1);
    std::wstring kind = vidpid.rfind("0b00", 0) == 0 ? L"蓝牙适配器" : L"USB HID";
    std::wstring wbusid = to_w(busid), wvid = to_w(vidpid), wst = L"已就绪";
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.pszText = wbusid.data();
    SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.iSubItem = 1;
    it.pszText = wvid.data();
    SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 2;
    it.pszText = kind.data();
    SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 3;
    it.pszText = wst.data();
    SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
    ++row;
  }
  refresh_counts();
}

void refresh_ble_list() {
  if (!g_ble_list) return;
  SendMessageW(g_ble_list, LB_RESETCONTENT, 0, 0);
  auto lines = pb::control::execute(g_sim, "ble-list");
  for (auto& l : lines) {
    if (l.rfind("OK", 0) == 0 || l.empty()) continue;
    std::wstring w = to_w(l);
    SendMessageW(g_ble_list, LB_ADDSTRING, 0, (LPARAM)w.c_str());
  }
  refresh_counts();
}

std::string pick_file() {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn = {sizeof(ofn)};
  ofn.hwndOwner = g_main;
  ofn.lpstrFilter = L"Profile (*.json)\0*.json\0所有文件\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn)) return pb::from_w(file);
  return "";
}

std::string selected_busid() {
  int sel = ListView_GetSelectionMark(g_list);
  if (sel < 0) return "";
  wchar_t buf[64] = {};
  LVITEMW it = {};
  it.iItem = sel;
  it.mask = LVIF_TEXT;
  it.pszText = buf;
  it.cchTextMax = 64;
  SendMessageW(g_list, LVM_GETITEMTEXTW, sel, (LPARAM)&it);
  return pb::from_w(std::wstring(buf));
}

void on_command(int id) {
  switch (id) {
    case IDC_NAV_DEVICE: select_page(0); return;
    case IDC_NAV_BLE: select_page(1); return;
    case IDC_NAV_LOG: select_page(2); return;
    case IDC_NAV_TEST: select_page(3); return;
    case IDC_BTN_RUNTESTS: start_auto_tests(); return;
    case IDC_BTN_ENVCHECK:
      pb::gui::show_env_dialog((HINSTANCE)GetWindowLongPtrW(g_main, GWLP_HINSTANCE), g_main);
      return;
  }
  switch (id) {
    case IDC_BTN_LOAD_HID: {
      auto f = pick_file();
      if (!f.empty()) {
        run_and_show("load-hid " + f);
        refresh_device_list();
      }
      break;
    }
    case IDC_BTN_LOAD_BT:
      run_and_show("load-bt");
      refresh_device_list();
      break;
    case IDC_BTN_ATTACH: {
      auto b = selected_busid();
      if (!b.empty()) run_and_show("attach " + b);
      else append_log_sync("> attach\nERR 先在列表中选择设备\n");
      break;
    }
    case IDC_BTN_DETACH: {
      auto b = selected_busid();
      if (!b.empty()) run_and_show("detach " + b);
      else append_log_sync("> detach\nERR 先在列表中选择设备\n");
      break;
    }
    case IDC_BTN_INJECT: {
      auto b = selected_busid();
      wchar_t hex[256] = {};
      GetWindowTextW(g_inject_edit, hex, 256);
      std::wstring wh(hex);
      std::string h(wh.size(), 0);
      for (size_t i = 0; i < wh.size(); ++i) h[i] = (char)wh[i];  // hex 输入是 ASCII
      // 空格剔除（用户可能按 "01 00" 输入）
      std::string clean;
      for (char c : h)
        if (c != ' ') clean.push_back(c);
      if (!b.empty() && !clean.empty()) run_and_show("inject " + b + " " + clean);
      else append_log_sync("> inject\nERR 需要选中设备并填写十六进制报告\n");
      break;
    }
    case IDC_BTN_BLE_ADV: {
      auto f = pick_file();
      if (!f.empty()) {
        run_and_show("ble-advertise " + f);
        refresh_ble_list();
      }
      break;
    }
    case IDC_BTN_BLE_STOP: {
      int sel = SendMessageW(g_ble_list, LB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        wchar_t buf[128] = {};
        SendMessageW(g_ble_list, LB_GETTEXT, sel, (LPARAM)buf);
        std::wstring w(buf);
        std::string line(w.begin(), w.end());
        size_t sp = line.find(' ');
        if (sp != std::string::npos) run_and_show("ble-stop " + line.substr(0, sp));
        refresh_ble_list();
      }
      break;
    }
    case IDC_BTN_TYPE: {
      wchar_t text[256] = {};
      GetWindowTextW(g_type_edit, text, 256);
      std::wstring w(text);
      std::string t(w.begin(), w.end());
      if (!t.empty()) run_and_show("ble-type C0:11:22:33:44:55 " + t);
      break;
    }
  }
}

// 页面容器：深色表面 + 子控件配色
LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      FillRect(dc, &rc, g_br_surface);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      HDC dc = (HDC)wp;
      SetBkColor(dc, pb::gui::BG);
      SetTextColor(dc, pb::gui::TEXT);
      return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = (HDC)wp;
      SetBkColor(dc, pb::gui::SURFACE);
      if ((HWND)lp == g_test_status) {
        SetTextColor(dc, g_test_status_color);
        return (LRESULT)g_br_surface;
      }
      bool is_empty_hint = (HWND)lp == g_empty_lbl;
      SetTextColor(dc, is_empty_hint ? RGB(0xB6, 0xBE, 0xC6) : pb::gui::TEXT_DIM);
      return (LRESULT)g_br_surface;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      auto* cs = (CREATESTRUCTW*)lp;
      pb::gui::register_pb_controls(cs->hInstance);
      pb::gui::enable_dark_titlebar(hwnd);

      int navw = pb::gui::scale(pb::gui::NAV_W);
      int apph = pb::gui::scale(pb::gui::APPBAR_H);
      int sth = pb::gui::scale(pb::gui::STATUS_H);
      auto mknav = [&](int id, const wchar_t* label, int y) {
        return CreateWindowExW(0, L"PbNavItem", label, WS_CHILD | WS_VISIBLE, 0, y, navw,
                               pb::gui::scale(40), hwnd, (HMENU)(INT_PTR)id, cs->hInstance, nullptr);
      };
      g_nav_dev = mknav(IDC_NAV_DEVICE, L"USB 设备", apph + pb::gui::scale(14));
      g_nav_ble = mknav(IDC_NAV_BLE, L"BLE 设备", apph + pb::gui::scale(58));
      g_nav_log = mknav(IDC_NAV_LOG, L"日志", apph + pb::gui::scale(102));
      g_nav_test = mknav(IDC_NAV_TEST, L"自动测试", apph + pb::gui::scale(146));

      RECT rc;
      GetClientRect(hwnd, &rc);
      int cw = rc.right - navw - pb::gui::scale(pb::gui::PAD);
      int ch = rc.bottom - apph - sth - pb::gui::scale(pb::gui::PAD) * 2;
      auto mkpage = [&](int id) {
        return CreateWindowExW(0, L"PbPage", L"", WS_CHILD,
                               navw + pb::gui::scale(pb::gui::PAD), apph + pb::gui::scale(pb::gui::PAD),
                               cw, ch, hwnd, (HMENU)(INT_PTR)id, cs->hInstance, nullptr);
      };
      g_page_dev = mkpage(1);
      g_page_ble = mkpage(2);
      g_page_log = mkpage(3);
      g_page_test = mkpage(4);

      int pad = pb::gui::scale(pb::gui::PAD);
      auto mkbtn = [&](int id, const wchar_t* text, int x, int y, int w, bool primary, HWND parent) {
        HWND b = CreateWindowExW(0, L"PbButton", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     (primary ? BS_DEFPUSHBUTTON : 0u),
                                 x, y, w, pb::gui::scale(32), parent, (HMENU)(INT_PTR)id,
                                 cs->hInstance, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)pb::gui::font_ui(), TRUE);
        return b;
      };

      // —— 页1：设备 ——
      g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                               WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, pad, pad,
                               cw - pad * 2, ch - pad * 2 - pb::gui::scale(104), g_page_dev,
                               nullptr, cs->hInstance, nullptr);
      ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT);
      SetWindowTheme(g_list, L"DarkMode_Explorer", nullptr);
      SendMessageW(g_list, WM_SETFONT, (WPARAM)pb::gui::font_mono(), TRUE);
      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (wchar_t*)L"busid";
      col.cx = pb::gui::scale(64);
      SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
      col.pszText = (wchar_t*)L"vid:pid";
      col.cx = pb::gui::scale(92);
      SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
      col.pszText = (wchar_t*)L"类型";
      col.cx = pb::gui::scale(110);
      SendMessageW(g_list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
      col.pszText = (wchar_t*)L"状态";
      col.cx = pb::gui::scale(92);
      SendMessageW(g_list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

      int by = ch - pad - pb::gui::scale(84);
      int bw = pb::gui::scale(118);
      mkbtn(IDC_BTN_LOAD_HID, L"加载 HID 设备", pad, by, bw, false, g_page_dev);
      mkbtn(IDC_BTN_LOAD_BT, L"加载蓝牙适配器", pad + bw + pad, by, bw, false, g_page_dev);
      mkbtn(IDC_BTN_ATTACH, L"附加设备", pad + (bw + pad) * 2, by, pb::gui::scale(84), true,
            g_page_dev);
      mkbtn(IDC_BTN_DETACH, L"分离设备", pad + (bw + pad) * 2 + pb::gui::scale(88), by,
            pb::gui::scale(84), false, g_page_dev);
      // 第二行：hex 输入（左对齐第一列）+ 发送紧随其后
      g_inject_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0100000000000000",
                                      WS_CHILD | WS_TABSTOP, pad, by + pb::gui::scale(44),
                                      pb::gui::scale(220), pb::gui::scale(28), g_page_dev, nullptr,
                                      cs->hInstance, nullptr);
      SendMessageW(g_inject_edit, WM_SETFONT, (WPARAM)pb::gui::font_mono(), TRUE);
      mkbtn(IDC_BTN_INJECT, L"发送报告", pad + pb::gui::scale(224), by + pb::gui::scale(42),
            pb::gui::scale(96), false, g_page_dev);
      // 空状态引导（有设备时隐藏）
      g_empty_lbl = CreateWindowExW(0, L"STATIC",
                                    L"未加载设备\n点击下方「加载 HID 设备」或「加载蓝牙适配器」",
                                    WS_CHILD | WS_VISIBLE | SS_CENTER, pad,
                                    pb::gui::scale(140), cw - pad * 2, pb::gui::scale(60),
                                    g_page_dev, nullptr, cs->hInstance, nullptr);
      SendMessageW(g_empty_lbl, WM_SETFONT, (WPARAM)pb::gui::font_ui(), TRUE);

      // —— 页2：BLE ——
      g_ble_list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | LBS_NOTIFY | WS_TABSTOP |
                                                              LBS_NOINTEGRALHEIGHT,
                                   pad, pad, cw - pad * 2, ch - pad * 2 - pb::gui::scale(112),
                                   g_page_ble, nullptr, cs->hInstance, nullptr);
      SendMessageW(g_ble_list, WM_SETFONT, (WPARAM)pb::gui::font_mono(), TRUE);
      mkbtn(IDC_BTN_BLE_ADV, L"加载 BLE 广播", pad, ch - pad - pb::gui::scale(84), bw, false,
            g_page_ble);
      mkbtn(IDC_BTN_BLE_STOP, L"停止选中", pad + bw + pad, ch - pad - pb::gui::scale(84),
            pb::gui::scale(96), false, g_page_ble);
      g_type_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"hello", WS_CHILD | WS_TABSTOP,
                                    pad, ch - pad - pb::gui::scale(40), pb::gui::scale(220),
                                    pb::gui::scale(28), g_page_ble, nullptr, cs->hInstance, nullptr);
      SendMessageW(g_type_edit, WM_SETFONT, (WPARAM)pb::gui::font_mono(), TRUE);
      mkbtn(IDC_BTN_TYPE, L"键入到 PhantomKb", pad + pb::gui::scale(228),
            ch - pad - pb::gui::scale(44), pb::gui::scale(160), true, g_page_ble);

      // —— 页3：日志 ——
      g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL | ES_READONLY,
                              pad, pad, cw - pad * 2, ch - pad * 2, g_page_log, nullptr,
                              cs->hInstance, nullptr);
      SendMessageW(g_log, WM_SETFONT, (WPARAM)pb::gui::font_mono(), TRUE);
      // 4 MB cap: lifts the default ~30k silent truncation while still
      // bounding the buffer (head-trim keeps steady state far below it).
      SendMessageW(g_log, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);

      // —— 页4：自动测试 ——
      g_test_btn = mkbtn(IDC_BTN_RUNTESTS, L"运行全部测试", pad, pad, pb::gui::scale(140), true,
                         g_page_test);
      mkbtn(IDC_BTN_ENVCHECK, L"环境体检", pad + pb::gui::scale(148), pad, pb::gui::scale(112),
            false, g_page_test);
      g_test_status = CreateWindowExW(0, L"STATIC", L"就绪 — 点击「运行全部测试」",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, pad + pb::gui::scale(272),
                                      pad + pb::gui::scale(6), pb::gui::scale(340),
                                      pb::gui::scale(24), g_page_test, nullptr, cs->hInstance,
                                      nullptr);
      g_test_status_color = pb::gui::TEXT_DIM;
      SendMessageW(g_test_status, WM_SETFONT, (WPARAM)pb::gui::font_ui_bold(), TRUE);
      g_test_out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL |
                                       ES_READONLY,
                                   pad, pad + pb::gui::scale(44), cw - pad * 2,
                                   ch - pad * 2 - pb::gui::scale(44), g_page_test, nullptr,
                                   cs->hInstance, nullptr);
      SendMessageW(g_test_out, WM_SETFONT, (WPARAM)pb::gui::font_mono(), TRUE);
      SendMessageW(g_test_out, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);

      select_page(0);
      refresh_device_list();
      break;
    }
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == 1) on_command(LOWORD(wp));
      if (HIWORD(wp) == LBN_DBLCLK) refresh_ble_list();
      break;
    case WM_LBUTTONDOWN: {
      // 手动体检入口：点击应用条 LED 区域
      int apph = pb::gui::scale(pb::gui::APPBAR_H);
      int x = LOWORD(lp), y = HIWORD(lp);
      if (y < apph && x > 0 && x < pb::gui::scale(40)) {
        pb::gui::show_env_dialog((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), hwnd);
      }
      break;
    }
    case WM_APP_LOG:
      append_log_sync((const char*)wp ? (const char*)wp : "");
      delete[](char*) wp;
      return 1;
    case WM_APP_TESTLINE:
      if (const char* line = (const char*)wp) {
        test_append(line);
        delete[] line;
      }
      return 1;
    case WM_APP_TESTDONE: {
      int rc = (int)wp;
      if (g_test_thread.joinable()) g_test_thread.join();
      EnableWindow(g_test_btn, TRUE);
      if (rc == -2) {
        test_set_status(L"测试超时或已中止（子进程被终止）", pb::gui::AMBER);
        return 1;
      }
      auto s = pb::testrunner::parse_summary(g_test_output);
      if (s.ran && s.all_green()) {
        wchar_t buf[96];
        swprintf(buf, 96, L"全部通过 %d/%d 项测试", s.passed, s.total);
        test_set_status(buf, pb::gui::ACCENT);
      } else if (s.ran) {
        wchar_t buf[96];
        swprintf(buf, 96, L"失败：%d/%d 通过（见下方输出）", s.passed, s.total);
        test_set_status(buf, pb::gui::DANGER);
      } else {
        wchar_t buf[96];
        swprintf(buf, 96, L"异常结束（退出码 %d，未解析到汇总行）", rc);
        test_set_status(buf, pb::gui::DANGER);
      }
      return 1;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      int apph = pb::gui::scale(pb::gui::APPBAR_H);
      int navw = pb::gui::scale(pb::gui::NAV_W);
      int sth = pb::gui::scale(pb::gui::STATUS_H);

      FillRect(dc, &rc, g_br_bg);
      RECT ab = {0, 0, rc.right, apph};
      FillRect(dc, &ab, g_br_surface);
      pb::gui::draw_hline(dc, 0, apph, rc.right, pb::gui::STROKE);

      SetBkMode(dc, TRANSPARENT);
      int led_r = pb::gui::scale(5);
      int led_cx = pb::gui::scale(22), led_cy = apph / 2;
      HBRUSH led = CreateSolidBrush(pb::gui::ACCENT);
      HBRUSH oldb = (HBRUSH)SelectObject(dc, led);
      Ellipse(dc, led_cx - led_r, led_cy - led_r, led_cx + led_r, led_cy + led_r);
      SelectObject(dc, oldb);
      DeleteObject(led);

      SetTextColor(dc, pb::gui::TEXT);
      HFONT old = (HFONT)SelectObject(dc, pb::gui::font_ui_bold());
      RECT tr = {led_cx + led_r + pb::gui::scale(10), 0, pb::gui::scale(300), apph};
      DrawTextW(dc, L"虚拟设备测试台", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, pb::gui::font_ui());
      SetTextColor(dc, pb::gui::TEXT_DIM);
      auto st = pb::control::execute(g_sim, "status");
      std::wstring s = st.empty() ? L"" : to_w(st[0].substr(3));
      for (auto& c : s)
        if (c == L' ') c = L'·';
      for (size_t i = 0; i + 5 < s.size(); ++i)
        if (s.compare(i, 5, L"port=") == 0) s.erase(i, 4);
      size_t dq = s.find(L"devices=");
      if (dq != std::wstring::npos) s.replace(dq, 8, L"设备 ");
      RECT sr = {rc.right - pb::gui::scale(340), 0, rc.right - pb::gui::scale(16), apph};
      DrawTextW(dc, s.c_str(), -1, &sr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);

      HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x3A, 0x41, 0x48));
      HPEN oldpen = (HPEN)SelectObject(dc, pen);
      MoveToEx(dc, navw, apph, nullptr);
      LineTo(dc, navw, rc.bottom - sth);
      SelectObject(dc, oldpen);
      DeleteObject(pen);

      RECT sb = {0, rc.bottom - sth, rc.right, rc.bottom};
      FillRect(dc, &sb, g_br_surface);
      pb::gui::draw_hline(dc, 0, rc.bottom - sth, rc.right, pb::gui::STROKE);
      SetTextColor(dc, pb::gui::TEXT_DIM);
      SelectObject(dc, pb::gui::font_ui());
      RECT fr = {pb::gui::scale(pb::gui::PAD), rc.bottom - sth, rc.right, rc.bottom};
      DrawTextW(dc, L"个人 / 非商业使用免费 · 商业使用需授权（LICENSE）", -1, &fr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);

      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SIZE: {
      // 内容区随窗口缩放
      int navw = pb::gui::scale(pb::gui::NAV_W);
      int apph = pb::gui::scale(pb::gui::APPBAR_H);
      int sth = pb::gui::scale(pb::gui::STATUS_H);
      int cw = LOWORD(lp) - navw - pb::gui::scale(pb::gui::PAD);
      int ch = HIWORD(lp) - apph - sth - pb::gui::scale(pb::gui::PAD) * 2;
      int px = navw + pb::gui::scale(pb::gui::PAD);
      int py = apph + pb::gui::scale(pb::gui::PAD);
      int pad = pb::gui::scale(pb::gui::PAD);
      MoveWindow(g_page_dev, px, py, cw, ch, TRUE);
      MoveWindow(g_page_ble, px, py, cw, ch, TRUE);
      MoveWindow(g_page_log, px, py, cw, ch, TRUE);
      MoveWindow(g_page_test, px, py, cw, ch, TRUE);
      if (g_test_out)
        MoveWindow(g_test_out, pad, pad + pb::gui::scale(44), cw - pad * 2,
                   ch - pad * 2 - pb::gui::scale(44), TRUE);
      if (g_test_status)
        MoveWindow(g_test_status, pad + pb::gui::scale(272), pad + pb::gui::scale(6),
                   cw - pad * 2 - pb::gui::scale(272), pb::gui::scale(24), TRUE);
      if (g_list) MoveWindow(g_list, pad, pad, cw - pad * 2, ch - pad * 2 - pb::gui::scale(104), TRUE);
      if (g_ble_list)
        MoveWindow(g_ble_list, pad, pad, cw - pad * 2, ch - pad * 2 - pb::gui::scale(112), TRUE);
      if (g_log) MoveWindow(g_log, pad, pad, cw - pad * 2, ch - pad * 2, TRUE);
      InvalidateRect(hwnd, nullptr, TRUE);
      break;
    }
    case WM_DESTROY:
      g_main = nullptr;  // worker thread checks IsWindow before SendMessage
      if (g_test_stop) SetEvent(g_test_stop);  // cancel any in-flight run; joined in wWinMain
      PostQuitMessage(0);
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto fn = (Fn)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
      fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  pb::gui::theme_init(nullptr);  // 屏幕取 DPI（创建窗口前）
  // （to_w 定义于匿名命名空间，此处 err 转换）

  std::string err;
  if (!g_sim.start(&err, 3240)) {
    MessageBoxW(nullptr, (L"Simulator 启动失败: " + to_w(err)).c_str(),
                L"phantom-bt", MB_ICONERROR);
    return 1;
  }
  static pb::control::PipeServer pipe(g_sim);
  g_pipe = &pipe;
  pipe.start(&err);

  g_br_surface = CreateSolidBrush(pb::gui::SURFACE);
  g_br_bg = CreateSolidBrush(pb::gui::BG);
  g_test_stop = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset

  pb::log_set_sink([](pb::LogLevel, std::string_view line) {
    if (!g_log || !g_main) return;
    std::string heap(line);
    heap += "\n";
    char* p = new char[heap.size() + 1];
    std::memcpy(p, heap.c_str(), heap.size() + 1);
    if (!SendMessageW(g_main, WM_APP_LOG, (WPARAM)p, 0)) delete[] p;
  });

  WNDCLASSW wc = {};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // res/app.rc: ID 1
  wc.lpszClassName = L"PhantomBTMain";
  RegisterClassW(&wc);
  WNDCLASSW pg = {};
  pg.lpfnWndProc = page_proc;
  pg.hInstance = inst;
  pg.lpszClassName = L"PbPage";
  RegisterClassW(&pg);

  g_main = CreateWindowExW(0, L"PhantomBTMain", L"phantom-bt · 虚拟设备测试台",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 700, nullptr,
                           nullptr, inst, nullptr);
  ShowWindow(g_main, show);
  UpdateWindow(g_main);
  // Small icon: pick the exact 16px frame from the .ico instead of letting the
  // system downscale the large one (title bar stays crisp).
  SendMessageW(g_main, WM_SETICON, ICON_SMALL,
               (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                  LR_DEFAULTCOLOR));

  refresh_device_list();

  // First launch: environment check (dialog guides through missing deps; silent + marked if all OK)
  if (!pb::env::first_run_done()) {
    PB_LOG_INFO("gui: first run — checking environment");
    auto checks = pb::env::run_checks();
    if (pb::env::needs_attention(checks)) {
      PB_LOG_INFO("gui: environment needs attention — showing dialog");
      pb::gui::show_env_dialog(inst, g_main, checks);  // reuse: no double probe
    }
    pb::env::mark_first_run();
  } else {
    PB_LOG_INFO("gui: not first run (env check skipped)");
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  pb::log_set_sink(nullptr);
  // Join the test worker before static destructors run (a detached worker
  // appending to g_test_output during process teardown is a UAF). The stop
  // event set in WM_DESTROY makes run_capture return quickly.
  if (g_test_thread.joinable()) g_test_thread.join();
  if (g_test_stop) CloseHandle(g_test_stop);
  pipe.stop();
  g_sim.stop();
  DeleteObject(g_br_surface);
  DeleteObject(g_br_bg);
  pb::gui::theme_shutdown();
  return (int)msg.wParam;
}
