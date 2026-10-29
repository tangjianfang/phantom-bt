// phantombt-gui.exe：Win32 GUI（内嵌 Simulator + 管道控制）
// 三页：设备（加载/attach/注入）、BLE（广播/停止）、日志（实时）
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "ble/ble_device.h"
#include "control/commands.h"
#include "control/pipe_server.h"
#include "core/log.h"
#include "sim/simulator.h"

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr int WM_APP_LOG = WM_APP + 1;

pb::sim::Simulator g_sim;
pb::control::PipeServer* g_pipe = nullptr;
HWND g_main = nullptr, g_tabs = nullptr, g_list = nullptr, g_log = nullptr;
HWND g_ble_list = nullptr, g_type_edit = nullptr;

void append_log_sync(const char* text) {
  if (!g_log) return;
  int len = GetWindowTextLengthA(g_log);
  SendMessageA(g_log, EM_SETSEL, len, len);
  SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
  // 环形上限：超长截前半
  if (len > 400000) {
    char head[2] = "";
    SendMessageA(g_log, EM_SETSEL, 0, len / 2);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)head);
  }
}

void refresh_device_list() {
  if (!g_list) return;
  SendMessageA(g_list, LVM_DELETEALLITEMS, 0, 0);
  auto lines = pb::control::execute(g_sim, "list");
  int row = 0;
  for (auto& l : lines) {
    if (l.rfind("DEV ", 0) != 0) continue;
    std::string rest = l.substr(4);
    LVITEMA it = {};
    it.mask = LVIF_TEXT;
    // busid vid:pid path
    size_t s1 = rest.find(' ');
    size_t s2 = rest.find(' ', s1 + 1);
    std::string busid = rest.substr(0, s1);
    std::string vidpid = rest.substr(s1 + 1, s2 - s1 - 1);
    std::string path = s2 == std::string::npos ? "" : rest.substr(s2 + 1);
    it.iItem = row;
    it.pszText = busid.data();
    SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    it.iSubItem = 1;
    it.pszText = vidpid.data();
    SendMessageA(g_list, LVM_SETITEMA, 0, (LPARAM)&it);
    it.iSubItem = 2;
    it.pszText = path.data();
    SendMessageA(g_list, LVM_SETITEMA, 0, (LPARAM)&it);
    ++row;
  }
}

void refresh_ble_list() {
  if (!g_ble_list) return;
  SendMessageA(g_ble_list, LB_RESETCONTENT, 0, 0);
  auto lines = pb::control::execute(g_sim, "ble-list");
  for (auto& l : lines) {
    if (l.rfind("OK", 0) == 0) continue;
    if (!l.empty()) SendMessageA(g_ble_list, LB_ADDSTRING, 0, (LPARAM)l.c_str());
  }
}

void run_and_show(const std::string& cmd) {
  auto replies = pb::control::execute(g_sim, cmd);
  std::string all;
  for (auto& r : replies) all += "> " + cmd + "\n" + r + "\n";
  append_log_sync(all.c_str());
}

std::string pick_file(const char* filter) {
  char file[MAX_PATH] = "";
  OPENFILENAMEA ofn = {sizeof(ofn)};
  ofn.hwndOwner = g_main;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameA(&ofn)) return file;
  return "";
}

void on_command(HWND hwnd, int id) {
  switch (id) {
    case 101: {  // 加载 HID Profile
      auto f = pick_file("HID Profile (*.json)\0*.json\0All\0*.*\0");
      if (!f.empty()) {
        run_and_show("load-hid " + f);
        refresh_device_list();
      }
      break;
    }
    case 102: {  // 加载蓝牙适配器
      run_and_show("load-bt");
      refresh_device_list();
      break;
    }
    case 103: {  // attach
      char busid[64] = {};
      LVITEMA it = {};
      it.iItem = 0;
      it.mask = LVIF_TEXT;
      it.pszText = busid;
      it.cchTextMax = 64;
      if (ListView_GetSelectionMark(g_list) >= 0) {
        it.iItem = ListView_GetSelectionMark(g_list);
        SendMessageA(g_list, LVM_GETITEMTEXTA, it.iItem, (LPARAM)&it);
        if (busid[0]) run_and_show(std::string("attach ") + busid);
      }
      break;
    }
    case 104: {  // 注入手柄报告（示例：按键1）
      run_and_show("inject 7-1 0100000000000000");
      break;
    }
    case 201: {  // BLE 广播
      auto f = pick_file("BLE Profile (*.json)\0*.json\0All\0*.*\0");
      if (!f.empty()) {
        run_and_show("ble-advertise " + f);
        refresh_ble_list();
      }
      break;
    }
    case 202: {  // BLE 停止选中
      int sel = SendMessageA(g_ble_list, LB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        char buf[128] = {};
        SendMessageA(g_ble_list, LB_GETTEXT, sel, (LPARAM)buf);
        std::string line = buf;
        size_t sp = line.find(' ');
        if (sp != std::string::npos) run_and_show("ble-stop " + line.substr(0, sp));
        refresh_ble_list();
      }
      break;
    }
    case 203: {  // 键入文本（BLE 键盘）
      char text[256] = {};
      GetWindowTextA(g_type_edit, text, 256);
      if (text[0]) run_and_show(std::string("ble-type C0:11:22:33:44:55 ") + text);
      break;
    }
  }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_tabs = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 900, 600, hwnd, (HMENU)1, nullptr, nullptr);
      TCITEMA ti = {};
      ti.mask = TCIF_TEXT;
      ti.pszText = (char*)"设备";
      SendMessageA(g_tabs, TCM_INSERTITEMA, 0, (LPARAM)&ti);
      ti.pszText = (char*)"BLE";
      SendMessageA(g_tabs, TCM_INSERTITEMA, 1, (LPARAM)&ti);
      ti.pszText = (char*)"日志";
      SendMessageA(g_tabs, TCM_INSERTITEMA, 2, (LPARAM)&ti);

      RECT rc;
      GetClientRect(g_tabs, &rc);
      SendMessageA(g_tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&rc);

      // 页1：设备
      g_list = CreateWindowExA(0, WC_LISTVIEWA, "",
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                               rc.left + 8, rc.top + 8, rc.right - rc.left - 16, 420, g_tabs,
                               nullptr, nullptr, nullptr);
      ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT);
      LVCOLUMNA col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (char*)"busid";
      col.cx = 70;
      SendMessageA(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      col.pszText = (char*)"vid:pid";
      col.cx = 100;
      SendMessageA(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
      col.pszText = (char*)"路径";
      col.cx = 380;
      SendMessageA(g_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
      CreateWindowExA(0, "BUTTON", "加载 HID Profile", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      rc.left + 8, rc.top + 440, 140, 30, g_tabs, (HMENU)101, nullptr, nullptr);
      CreateWindowExA(0, "BUTTON", "加载蓝牙适配器", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      rc.left + 156, rc.top + 440, 140, 30, g_tabs, (HMENU)102, nullptr, nullptr);
      CreateWindowExA(0, "BUTTON", "Attach 选中", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      rc.left + 304, rc.top + 440, 120, 30, g_tabs, (HMENU)103, nullptr, nullptr);
      CreateWindowExA(0, "BUTTON", "注入示例报告", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      rc.left + 432, rc.top + 440, 120, 30, g_tabs, (HMENU)104, nullptr, nullptr);

      // 页2：BLE
      g_ble_list = CreateWindowExA(0, "LISTBOX", "",
                                   WS_CHILD | LBS_NOTIFY, rc.left + 8, rc.top + 8,
                                   rc.right - rc.left - 16, 300, g_tabs, nullptr, nullptr, nullptr);
      CreateWindowExA(0, "BUTTON", "BLE 广播", WS_CHILD | BS_PUSHBUTTON, rc.left + 8, rc.top + 320,
                      120, 30, g_tabs, (HMENU)201, nullptr, nullptr);
      CreateWindowExA(0, "BUTTON", "停止选中", WS_CHILD | BS_PUSHBUTTON, rc.left + 136,
                      rc.top + 320, 120, 30, g_tabs, (HMENU)202, nullptr, nullptr);
      g_type_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD, rc.left + 8,
                                    rc.top + 360, 300, 26, g_tabs, nullptr, nullptr, nullptr);
      CreateWindowExA(0, "BUTTON", "键入文本(PhantomKb)", WS_CHILD | BS_PUSHBUTTON, rc.left + 316,
                      rc.top + 360, 170, 26, g_tabs, (HMENU)203, nullptr, nullptr);

      // 页3：日志
      g_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                              WS_CHILD | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL | ES_READONLY,
                              rc.left + 8, rc.top + 8, rc.right - rc.left - 16,
                              rc.bottom - rc.top - 16, g_tabs, nullptr, nullptr, nullptr);
      SendMessageA(g_tabs, TCM_SETCURSEL, 0, 0);
      break;
    }
    case WM_NOTIFY: {
      auto* nm = (NMHDR*)lp;
      if (nm->hwndFrom == g_tabs && nm->code == TCN_SELCHANGE) {
        int sel = SendMessageA(g_tabs, TCM_GETCURSEL, 0, 0);
        ShowWindow(g_list, sel == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(g_ble_list, sel == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(g_type_edit, sel == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(g_log, sel == 2 ? SW_SHOW : SW_HIDE);
        // 页1/页2 的按钮们
        HWND child = GetWindow(g_tabs, GW_CHILD);
        while (child) {
          char cls[16];
          GetClassNameA(child, cls, 16);
          if (lstrcmpA(cls, "Button") == 0) {
            int id = GetDlgCtrlID(child);
            bool page1 = id >= 101 && id <= 104;
            bool page2 = id >= 201 && id <= 203;
            ShowWindow(child, (sel == 0 && page1) || (sel == 1 && page2) ? SW_SHOW : SW_HIDE);
          }
          child = GetWindow(child, GW_HWNDNEXT);
        }
      }
      break;
    }
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED) on_command(hwnd, LOWORD(wp));
      if (HIWORD(wp) == LBN_DBLCLK) refresh_ble_list();
      break;
    case WM_APP_LOG:
      append_log_sync((const char*)wp ? (const char*)wp : "");
      delete[] (char*)wp;
      return 1;
    case WM_DESTROY:
      PostQuitMessage(0);
      break;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
  InitCommonControlsEx(&icc);

  std::string err;
  if (!g_sim.start(&err, 3240)) {
    MessageBoxA(nullptr, ("Simulator 启动失败: " + err).c_str(), "Phantom-BT", MB_ICONERROR);
    return 1;
  }
  static pb::control::PipeServer pipe(g_sim);
  g_pipe = &pipe;
  pipe.start(&err);

  // 日志 → GUI（后台线程安全：经 SendMessage 同步）
  pb::log_set_sink([](pb::LogLevel, std::string_view line) {
    if (!g_log) return;
    std::string heap(line);
    heap += "\n";
    char* p = new char[heap.size() + 1];
    std::memcpy(p, heap.c_str(), heap.size() + 1);
    if (!SendMessageA(g_main, WM_APP_LOG, (WPARAM)p, 0)) {
      delete[] p;  // 窗口销毁后
    }
  });

  WNDCLASSA wc = {};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = "PhantomBTMain";
  RegisterClassA(&wc);
  g_main = CreateWindowExA(0, "PhantomBTMain", "Phantom-BT 虚拟设备测试台",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 920, 660, nullptr,
                           nullptr, inst, nullptr);
  ShowWindow(g_main, show);
  UpdateWindow(g_main);

  refresh_device_list();

  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  pb::log_set_sink(nullptr);
  pipe.stop();
  g_sim.stop();
  return (int)msg.wParam;
}
