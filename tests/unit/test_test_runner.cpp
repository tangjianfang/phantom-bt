#include "test/test_framework.h"

#include <windows.h>

#include "core/test_runner.h"

#include <string>
#include <vector>

using namespace pb;

PB_TEST(tr_parse_summary_valid) {
  auto s = testrunner::parse_summary("[x] ok\n== 161/161 tests passed ==\n");
  PB_CHECK(s.ran);
  PB_CHECK_EQ(s.passed, 161);
  PB_CHECK_EQ(s.total, 161);
  PB_CHECK(s.all_green());
}

PB_TEST(tr_parse_summary_failures) {
  auto s = testrunner::parse_summary("== 3/5 tests passed ==\n");
  PB_CHECK(s.ran);
  PB_CHECK_EQ(s.passed, 3);
  PB_CHECK_EQ(s.total, 5);
  PB_CHECK(!s.all_green());
}

PB_TEST(tr_parse_summary_absent) {
  PB_CHECK(!testrunner::parse_summary("").ran);
  PB_CHECK(!testrunner::parse_summary("some random output\n").ran);
  PB_CHECK(!testrunner::parse_summary("tests passed == without opener").ran);
}

PB_TEST(tr_parse_summary_takes_last) {
  // Multiple summary lines (shouldn't happen, but take the last one).
  auto s = testrunner::parse_summary("== 1/2 tests passed ==\n== 7/9 tests passed ==\n");
  PB_CHECK(s.ran);
  PB_CHECK_EQ(s.passed, 7);
  PB_CHECK_EQ(s.total, 9);
}

PB_TEST(tr_find_tests_exe_finds_itself) {
  // While the unit tests run, phantombt-tests.exe is exactly the binary
  // find_tests_exe() should locate.
  std::string exe = testrunner::find_tests_exe();
  PB_CHECK(!exe.empty());
}

PB_TEST(tr_run_capture_lines_and_exit_code) {
  // Run a harmless child (cmd echo) instead of the test binary itself to
  // avoid recursion. Verify line callbacks and the exit code.
  std::vector<std::string> lines;
  int rc = testrunner::run_capture("cmd.exe /c echo alpha && echo bravo",
                                   [&](const std::string& l) { lines.push_back(l); });
  PB_CHECK_EQ(rc, 0);
  PB_CHECK(lines.size() >= 2);
  bool has_alpha = false, has_bravo = false;
  for (auto& l : lines) {
    if (l.find("alpha") != std::string::npos) has_alpha = true;
    if (l.find("bravo") != std::string::npos) has_bravo = true;
  }
  PB_CHECK(has_alpha && has_bravo);
}

PB_TEST(tr_run_capture_exit_code_propagates) {
  int rc = testrunner::run_capture("cmd.exe /c exit 3", nullptr);
  PB_CHECK_EQ(rc, 3);
}

PB_TEST(tr_run_capture_missing_exe) {
  // Wrapped in cmd.exe: a missing command makes cmd exit non-zero (1),
  // not -1 (-1 is only when cmd.exe itself fails to launch).
  std::string seen;
  int rc = testrunner::run_capture("definitely-not-a-real-exe-xyz.exe",
                                   [&](const std::string& l) { seen += l + "\n"; });
  PB_CHECK(rc != 0);
  PB_CHECK(seen.find("not recognized") != std::string::npos ||
           seen.find("is not recognized") != std::string::npos || seen.find("'") != std::string::npos);
}

PB_TEST(tr_run_capture_stop_event_cancels) {
  // A long-running child + an already-set stop event must return -2 quickly
  // (the child gets terminated instead of blocking for its full runtime).
  HANDLE stop = CreateEventW(nullptr, FALSE, TRUE, nullptr);  // manual set
  PB_CHECK(stop != nullptr);
  ULONGLONG t0 = GetTickCount64();
  int rc = testrunner::run_capture("cmd.exe /c ping -n 30 127.0.0.1 >nul", nullptr, stop, 30000);
  ULONGLONG dt = GetTickCount64() - t0;
  CloseHandle(stop);
  PB_CHECK_EQ(rc, -2);
  PB_CHECK(dt < 10000);  // cancelled, not the ~30s ping runtime
}

PB_TEST(tr_run_capture_timeout_kills_hung_child) {
  // No stop event: the hard timeout must fire and kill the child.
  ULONGLONG t0 = GetTickCount64();
  int rc = testrunner::run_capture("cmd.exe /c ping -n 30 127.0.0.1 >nul", nullptr, nullptr, 1500);
  ULONGLONG dt = GetTickCount64() - t0;
  PB_CHECK_EQ(rc, -2);
  PB_CHECK(dt >= 1400 && dt < 15000);
}
