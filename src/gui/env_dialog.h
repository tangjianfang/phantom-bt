#pragma once
// 环境体检对话框：列出依赖检测结果 + 官方下载页跳转 + 重新检测
#include <windows.h>

#include <vector>

#include "core/env_check.h"

namespace pb::gui {

// Show the (modeless) env-check dialog. A non-empty `checks` vector is used
// as-is (the first-run path already probed once — avoids a second
// SCManager/registry/PATH scan); an empty one probes on the spot.
void show_env_dialog(HINSTANCE inst, HWND owner,
                     const std::vector<env::CheckResult>& checks = {});

}  // namespace pb::gui
