#include "gui/theme.h"

#include <commctrl.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace pb::gui {

namespace {
int g_dpi = 96;
HFONT g_font_ui = nullptr, g_font_ui_bold = nullptr, g_font_mono = nullptr, g_font_title = nullptr;

int pt_to_px(int pt) { return MulDiv(pt, g_dpi, 72); }

struct BtnState {
  bool hover = false;
  bool pressed = false;
};

LRESULT CALLBACK pb_button_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* st = (BtnState*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (msg) {
    case WM_NCCREATE:
      st = new BtnState();
      SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
      break;
    case WM_NCDESTROY:
      delete st;
      break;
    case WM_MOUSEMOVE: {
      if (st) st->hover = true;
      TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
      _TrackMouseEvent(&t);
      InvalidateRect(h, nullptr, TRUE);
      break;
    }
    case WM_MOUSELEAVE:
      if (st) st->hover = false;
      InvalidateRect(h, nullptr, TRUE);
      break;
    case WM_LBUTTONDOWN:
      SetCapture(h);
      if (st) st->pressed = true;
      InvalidateRect(h, nullptr, TRUE);
      break;
    case WM_LBUTTONUP: {
      if (GetCapture() == h) ReleaseCapture();
      if (st) st->pressed = false;
      InvalidateRect(h, nullptr, TRUE);
      POINT pt = {LOWORD(lp), HIWORD(lp)};
      RECT rc;
      GetClientRect(h, &rc);
      if (PtInRect(&rc, pt) && wp & MK_LBUTTON || PtInRect(&rc, pt)) {
        HWND parent = GetParent(h);
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
      }
      break;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      bool primary = (GetWindowLongPtrW(h, GWL_STYLE) & BS_DEFPUSHBUTTON) != 0;
      COLORREF border = primary ? ACCENT : STROKE;
      COLORREF fill = st && st->hover ? SURFACE_HI : SURFACE;
      if (st && st->pressed) fill = ACCENT_DIM;
      COLORREF txt = primary ? ACCENT : TEXT;

      HBRUSH br = CreateSolidBrush(fill);
      HBRUSH bo = CreateSolidBrush(border);
      HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
                                    scale(6), scale(6));
      FillRgn(dc, rgn, br);
      FrameRgn(dc, rgn, bo, 1, 1);
      DeleteObject(rgn);
      DeleteObject(br);
      DeleteObject(bo);

      wchar_t text[128];
      GetWindowTextW(h, text, 128);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, txt);
      HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
      HFONT old = (HFONT)SelectObject(dc, f ? f : font_ui());
      RECT trc = rc;
      DrawTextW(dc, text, -1, &trc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      SelectObject(dc, old);
      EndPaint(h, &ps);
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

struct NavState {
  bool hover = false;
  bool selected = false;
  int badge = 0;
};

LRESULT CALLBACK pb_nav_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* st = (NavState*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (msg) {
    case WM_NCCREATE:
      st = new NavState();
      SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
      break;
    case WM_NCDESTROY:
      delete st;
      break;
    case PB_NAV_SELECT:
      if (st) {
        st->selected = (wp != 0);
        InvalidateRect(h, nullptr, TRUE);
      }
      return 0;
    case PB_NAV_BADGE:
      if (st) {
        st->badge = (int)wp;
        InvalidateRect(h, nullptr, TRUE);
      }
      return 0;
    case WM_MOUSEMOVE: {
      if (st) st->hover = true;
      TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
      _TrackMouseEvent(&t);
      InvalidateRect(h, nullptr, TRUE);
      break;
    }
    case WM_MOUSELEAVE:
      if (st) st->hover = false;
      InvalidateRect(h, nullptr, TRUE);
      break;
    case WM_LBUTTONUP: {
      HWND parent = GetParent(h);
      SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), 1 /*nav*/), (LPARAM)h);
      break;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      COLORREF fill = st && st->selected ? SURFACE_HI
                                         : (st && st->hover ? RGB(0x1B, 0x1E, 0x23) : BG);
      HBRUSH br = CreateSolidBrush(fill);
      FillRect(dc, &rc, br);
      DeleteObject(br);
      if (st && st->selected) {
        HBRUSH bar = CreateSolidBrush(ACCENT);
        RECT lb = {rc.left, rc.top + 4, rc.left + 3, rc.bottom - 4};
        FillRect(dc, &lb, bar);
        DeleteObject(bar);
      }
      wchar_t label[64];
      GetWindowTextW(h, label, 64);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, st && st->selected ? TEXT : TEXT_DIM);
      HFONT old = (HFONT)SelectObject(dc, st && st->selected ? font_ui_bold() : font_ui());
      RECT trc = {rc.left + scale(14), rc.top, rc.right - scale(30), rc.bottom};
      DrawTextW(dc, label, -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);
      if (st && st->badge > 0) {
        wchar_t b[8];
        _snwprintf(b, 8, L"%d", st->badge);
        int r = scale(9);
        int cx = rc.right - r - scale(6);
        int cy = (rc.top + rc.bottom) / 2;
        HBRUSH dot = CreateSolidBrush(st->selected ? ACCENT : RGB(0x3A, 0x41, 0x48));
        HBRUSH oldb = (HBRUSH)SelectObject(dc, dot);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, oldb);
        DeleteObject(dot);
        SetTextColor(dc, BG);
        RECT brc = {cx - r, cy - r - 1, cx + r, cy + r};
        DrawTextW(dc, b, -1, &brc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      EndPaint(h, &ps);
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void theme_init(HWND hwnd) {
  HDC dc = GetDC(hwnd);
  g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(hwnd, dc);
  g_font_ui = CreateFontW(-pt_to_px(9), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  g_font_ui_bold = CreateFontW(-pt_to_px(9), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  g_font_mono = CreateFontW(-pt_to_px(9), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
  g_font_title = CreateFontW(-pt_to_px(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

HFONT font_ui() { return g_font_ui; }
HFONT font_ui_bold() { return g_font_ui_bold; }
HFONT font_mono() { return g_font_mono; }
HFONT font_title() { return g_font_title; }

void theme_shutdown() {
  for (HFONT f : {g_font_ui, g_font_ui_bold, g_font_mono, g_font_title})
    if (f) DeleteObject(f);
  g_font_ui = g_font_ui_bold = g_font_mono = g_font_title = nullptr;
}

int scale(int px) { return MulDiv(px, g_dpi, 96); }

void enable_dark_titlebar(HWND hwnd) {
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
}

void draw_hline(HDC dc, int x1, int y, int x2, COLORREF c) {
  HPEN pen = CreatePen(PS_SOLID, 1, c);
  HPEN old = (HPEN)SelectObject(dc, pen);
  MoveToEx(dc, x1, y, nullptr);
  LineTo(dc, x2, y);
  SelectObject(dc, old);
  DeleteObject(pen);
}

void register_pb_controls(HINSTANCE inst) {
  WNDCLASSW b = {};
  b.lpfnWndProc = pb_button_proc;
  b.hInstance = inst;
  b.hCursor = LoadCursor(nullptr, IDC_HAND);
  b.lpszClassName = L"PbButton";
  RegisterClassW(&b);
  WNDCLASSW n = {};
  n.lpfnWndProc = pb_nav_proc;
  n.hInstance = inst;
  n.hCursor = LoadCursor(nullptr, IDC_HAND);
  n.lpszClassName = L"PbNavItem";
  RegisterClassW(&n);
}

}  // namespace pb::gui
