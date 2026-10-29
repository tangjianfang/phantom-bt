#include "test/test_framework.h"
#include <windows.h>

int main(int argc, char** argv) {
  SetCurrentDirectoryA(PB_SOURCE_DIR);  // 测试以仓库根为工作目录（读取 profiles/ 等）
  return pb::TestRegistry::instance().run_all(argc > 1 ? argv[1] : "");
}
