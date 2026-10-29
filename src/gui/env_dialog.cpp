#include "gui/env_dialog.h"

#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "core/bytes.h"
#include "core/env_check.h"
#include "core/log.h"
#include "gui/theme.h"

namespace pb::gui {

namespace {

std::vector<env::CheckResult> g_checks;
std::vector<int> g_row_h;  // 每行自适应高度
HWND g_dlg = nullptr;

constexpr int DLG_W_BASE = 680;
constexpr int TXT_X_BASE = 44;    // 文本列起点（状态灯右）
constexpr int BTN_X_BASE = 470;   // 操作按钮列
constexpr int TXT_W_BASE = 400;   // 文本列宽（BTN_X - TXT_X - 12）

std::wstring to_w(const std::string& s) { return pb::to_w(s); }

// 自适应行高：标题 26 + 说明实际高度 + 上下留白
int measure_row(HDC dc, const env::CheckResult& c) {
  std::wstring d = to_w(c.detail);
  RECT r = {0, 0, scale(TXT_W_BASE), 0};
  HFONT old = (HFONT)SelectObject(dc, font_ui());
  DrawTextW(dc, d.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT);
  int dh = r.bottom - r.top;
  SelectObject(dc, old);
  return scale(30) + dh + scale(18);
}

int total_height() {
  int h = scale(64);  // 标题区
  for (int rh : g_row_h) h += rh;
  h += scale(20) + scale(44);  // 底部按钮区
  return h;
}

void rebuild_rows(HWND dlg) {
  for (HWND child = GetWindow(dlg, GW_CHILD); child;)
    {
      HWND next = GetWindow(child, GW_HWNDNEXT);
      char cls[16];
      GetClassNameA(child, cls, 16);
      if (lstrcmpA(cls, "PbButton") == 0) DestroyWindow(child);
      child = next;
    }
  g_row_h.clear();
  HDC dc = GetDC(dlg);
  for (auto& c : g_checks) g_row_h.push_back(measure_row(dc, c));
  ReleaseDC(dlg, dc);

  HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(dlg, GWLP_HINSTANCE);
  int y = scale(64);
  for (size_t i = 0; i < g_checks.size(); ++i) {
    if (!g_checks[i].action_url.empty()) {
      std::wstring label = to_w(g_checks[i].action_label);
      HWND b = CreateWindowExW(0, L"PbButton", label.c_str(), WS_CHILD | WS_VISIBLE,
                               scale(BTN_X_BASE), y + scale(10), scale(178), scale(32), dlg,
                               (HMENU)(INT_PTR)(9000 + i), inst, nullptr);
      SendMessageW(b, WM_SETFONT, (WPARAM)font_ui(), TRUE);
    }
    y += g_row_h[i];
  }
  // 底部按钮
  int h = total_height();
  HWND again = CreateWindowExW(0, L"PbButton", L"重新检测", WS_CHILD | WS_VISIBLE, scale(20),
                               h - scale(52), scale(112), scale(32), dlg, (HMENU)(INT_PTR)9100,
                               inst, nullptr);
  SendMessageW(again, WM_SETFONT, (WPARAM)font_ui(), TRUE);
  HWND close = CreateWindowExW(0, L"PbButton", L"关闭",
                               WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, scale(DLG_W_BASE) - scale(132),
                               h - scale(52), scale(112), scale(32), dlg, (HMENU)(INT_PTR)9101,
                               inst, nullptr);
  SendMessageW(close, WM_SETFONT, (WPARAM)font_ui(), TRUE);

  RECT rc = {0, 0, scale(DLG_W_BASE), h};
  AdjustWindowRectEx(&rc, WS_POPUPWINDOW | WS_CAPTION | WS_MINIMIZEBOX, FALSE, WS_EX_TOOLWINDOW);
  SetWindowPos(dlg, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
               SWP_NOMOVE | SWP_NOZORDER);
  InvalidateRect(dlg, nullptr, TRUE);
}

void open_url(const std::string& url) {
  std::wstring w = to_w(url);
  ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HBRUSH bg = CreateSolidBrush(BG);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      SetBkMode(dc, TRANSPARENT);

      SetTextColor(dc, TEXT);
      HFONT old = (HFONT)SelectObject(dc, font_title());
      RECT t = {scale(20), scale(12), rc.right - scale(16), scale(40)};
      DrawTextW(dc, L"环境体检", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, font_ui());
      SetTextColor(dc, TEXT_DIM);
      RECT sub = {scale(20), scale(38), rc.right - scale(16), scale(60)};
      DrawTextW(dc, L"虚拟设备测试台的运行依赖状态。首次启动自动检测，之后可点击主窗口左上角状态灯重新打开。",
                -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

      int y = scale(64);
      for (size_t i = 0; i < g_checks.size(); ++i) {
        const auto& c = g_checks[i];
        int rh = g_row_h[i];
        // 状态灯
        COLORREF dot = c.status == env::Status::Ok
                           ? ACCENT
                           : (c.status == env::Status::Warn ? AMBER : DANGER);
        HBRUSH b = CreateSolidBrush(dot);
        HBRUSH ob = (HBRUSH)SelectObject(dc, b);
        int r = scale(5), cx = scale(28), cy = y + scale(17);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, ob);
        DeleteObject(b);
        // 标题（一行）+ 说明（自适应多行，止于按钮列）
        SetTextColor(dc, TEXT);
        SelectObject(dc, font_ui_bold());
        RECT rt = {scale(TXT_X_BASE), y + scale(4), scale(TXT_X_BASE + TXT_W_BASE), y + scale(28)};
        DrawTextW(dc, to_w(c.title).c_str(), -1, &rt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, font_ui());
        SetTextColor(dc, TEXT_DIM);
        RECT rd = {scale(TXT_X_BASE), y + scale(30), scale(TXT_X_BASE + TXT_W_BASE),
                   y + rh - scale(6)};
        DrawTextW(dc, to_w(c.detail).c_str(), -1, &rd, DT_LEFT | DT_TOP | DT_WORDBREAK);
        draw_hline(dc, scale(16), y + rh - 1, rc.right - scale(16), STROKE);
        y += rh;
      }
      SelectObject(dc, old);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (id >= 9000 && id < 9000 + (int)g_checks.size()) {
        open_url(g_checks[id - 9000].action_url);
      } else if (id == 9100) {
        g_checks = env::run_checks();
        rebuild_rows(h);
      } else if (id == 9101) {
        DestroyWindow(h);
      }
      break;
    }
    case WM_DESTROY:
      g_dlg = nullptr;
      break;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void ensure_class(HINSTANCE inst) {
  static bool done = false;
  if (done) return;
  WNDCLASSW wc = {};
  wc.lpfnWndProc = dlg_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // res/app.rc: ID 1
  wc.lpszClassName = L"PbCheckDlg";
  RegisterClassW(&wc);
  done = true;
}

}  // namespace

void show_env_dialog(HINSTANCE inst, HWND owner) {
  ensure_class(inst);
  if (g_dlg) {
    SetForegroundWindow(g_dlg);
    return;
  }
  g_checks = env::run_checks();
  int h = scale(64) + scale(70) * (int)g_checks.size() + scale(64);  // 初始估计，rebuild 校正
  g_dlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"PbCheckDlg", L"phantom-bt · 环境体检",
                          WS_POPUPWINDOW | WS_CAPTION | WS_MINIMIZEBOX, CW_USEDEFAULT,
                          CW_USEDEFAULT, scale(DLG_W_BASE), h, owner, nullptr, inst, nullptr);
  enable_dark_titlebar(g_dlg);
  g_row_h.assign(g_checks.size(), scale(70));
  rebuild_rows(g_dlg);
  ShowWindow(g_dlg, SW_SHOW);
  SetForegroundWindow(g_dlg);
}

}  // namespace pb::gui
