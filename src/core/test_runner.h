#pragma once
// Test runner helpers shared by the GUI "auto test" page and unit tests:
// locate the test binary, capture its output line by line, parse the summary.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace pb::testrunner {

// Locate phantombt-tests.exe relative to the running executable:
// same dir (release/tests layout puts them one level apart), sibling "tests/"
// dir, and the debug build tree. Empty string when not found.
std::string find_tests_exe();

struct Summary {
  bool ran = false;    // a "== p/t tests passed ==" line was found
  int passed = 0;
  int total = 0;
  bool all_green() const { return ran && passed == total; }
};

// Parse the summary line from a captured test run output.
Summary parse_summary(const std::string& output);

// Run `cmdline` (e.g. the tests exe path, quoted), invoke `on_line` per output
// line (without the trailing newline), return the process exit code.
// `stop` (optional, auto-reset event): when set, the child is terminated and
// -2 is returned. A run is also capped at `timeout_ms` (child terminated,
// -2 returned). -1 means the child failed to launch.
int run_capture(const std::string& cmdline, const std::function<void(const std::string&)>& on_line,
                HANDLE stop = nullptr, DWORD timeout_ms = 300000);

}  // namespace pb::testrunner
