#pragma once
// 环境体检对话框：列出依赖检测结果 + 官方下载页跳转 + 重新检测
#include <windows.h>

namespace pb::gui {

// 显示（非模态）体检窗口；现场执行检测
void show_env_dialog(HINSTANCE inst, HWND owner);

}  // namespace pb::gui
