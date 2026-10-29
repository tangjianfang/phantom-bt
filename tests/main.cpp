#include "test/test_framework.h"
#include <windows.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  _set_error_mode(_OUT_TO_STDERR);  // 越界等运行时错误走 stderr，禁止后台弹窗挂起
  SetCurrentDirectoryA(PB_SOURCE_DIR);  // 测试以仓库根为工作目录（读取 profiles/ 等）
  return pb::TestRegistry::instance().run_all(argc > 1 ? argv[1] : "");
}
