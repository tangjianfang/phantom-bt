#pragma once
// phantom-bt GUI 主题层：仪器面板配色 / 字体 / 自绘控件（PbButton、PbNavItem）
// 设计 token：
//   面板底 #17191D（阳极氧化石墨）  表层 #1F2328  描边 #2E343B
//   文本 #DDE3E8 / 次文本 #8B949C   强调 #3FD0B6（磷光青 LED）
//   琥珀 #E5A54B（广播/警示）        错误 #E06C5F
#include <windows.h>
#include <string>

namespace pb::gui {

// —— 色板 ——
constexpr COLORREF BG = RGB(0x17, 0x19, 0x1D);        // 面板底
constexpr COLORREF SURFACE = RGB(0x1F, 0x23, 0x28);   // 表层（列表/输入）
constexpr COLORREF SURFACE_HI = RGB(0x27, 0x2C, 0x33);  // hover 表层
constexpr COLORREF STROKE = RGB(0x2E, 0x34, 0x3B);    // 描边
constexpr COLORREF TEXT = RGB(0xDD, 0xE3, 0xE8);      // 主文本
constexpr COLORREF TEXT_DIM = RGB(0x8B, 0x94, 0x9C);  // 次文本
constexpr COLORREF ACCENT = RGB(0x3F, 0xD0, 0xB6);    // 磷光青
constexpr COLORREF AMBER = RGB(0xE5, 0xA5, 0x4B);     // 琥珀
constexpr COLORREF DANGER = RGB(0xE0, 0x6C, 0x5F);    // 错误
constexpr COLORREF ACCENT_DIM = RGB(0x27, 0x4A, 0x43);  // 强调色暗底

// —— 布局常量（96DPI 基准，运行时按 DPI 缩放）——
constexpr int APPBAR_H = 52;
constexpr int NAV_W = 152;
constexpr int STATUS_H = 26;
constexpr int PAD = 12;

// 字体（DPI 缩放，懒创建；main 退出时清理）
void theme_init(HWND hwnd);
HFONT font_ui();        // Segoe UI 9pt
HFONT font_ui_bold();   // Segoe UI Semibold 9.5pt
HFONT font_mono();      // Cascadia Mono 9pt（数据/日志）
HFONT font_title();     // Segoe UI Semibold 12pt（应用条标题）
void theme_shutdown();
int scale(int px);      // DPI 缩放

// 暗标题栏（Win10 1809+；失败静默）
void enable_dark_titlebar(HWND hwnd);

// 自绘按钮：注册 + 窗口过程。文本色 TEXT，描边 STROKE，hover SURFACE_HI，
// 主按钮（BS_DEFPUSHBUTTON 语义）描边/文字 ACCENT
void register_pb_controls(HINSTANCE inst);
// 自绘导航项：WM_USER+1 设选中，WM_USER+2 设徽标计数（int）
constexpr UINT PB_NAV_SELECT = WM_USER + 1;
constexpr UINT PB_NAV_BADGE = WM_USER + 2;

// 常用绘制工具
void draw_hline(HDC dc, int x1, int y, int x2, COLORREF c);

}  // namespace pb::gui
