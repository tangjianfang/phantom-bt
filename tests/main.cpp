#include "test/test_framework.h"
#include <windows.h>
#include <crtdbg.h>
#include <stdio.h>
#include <stdlib.h>

// 崩溃/断言自报告：全部走 stderr，禁止系统弹窗（后台无人点击会挂死）
static void install_silent_crash_reporting() {
  _set_error_mode(_OUT_TO_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
    std::fprintf(stderr, "\nCRASH: exception 0x%08lX at %p\n",
                 e ? e->ExceptionRecord->ExceptionCode : 0,
                 e ? e->ExceptionRecord->ExceptionAddress : nullptr);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;  // 静默终止（不弹 WER 对话框）
  });
}

int main(int argc, char** argv) {
  install_silent_crash_reporting();
  SetCurrentDirectoryA(PB_SOURCE_DIR);  // 测试以仓库根为工作目录（读取 profiles/ 等）
  return pb::TestRegistry::instance().run_all(argc > 1 ? argv[1] : "");
}
