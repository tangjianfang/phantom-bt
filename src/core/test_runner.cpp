#include "core/test_runner.h"

#include <windows.h>

#include <string>
#include <vector>

#include "core/bytes.h"

namespace pb::testrunner {

namespace {

bool file_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring exe_dir() {
  wchar_t self[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return L"";
  std::wstring dir(self);
  size_t slash = dir.find_last_of(L'\\');
  if (slash == std::wstring::npos) return L"";
  return dir.substr(0, slash + 1);  // trailing backslash included
}

}  // namespace

std::string find_tests_exe() {
  // Layout produced by the presets: build/<cfg>/src/phantombt-gui.exe and
  // build/<cfg>/tests/phantombt-tests.exe, so from the GUI exe it is
  // ..\tests\phantombt-tests.exe. Also try the same dir and the debug tree.
  std::wstring dir = exe_dir();
  if (dir.empty()) return "";
  const wchar_t* kCandidates[] = {
      L"..\\tests\\phantombt-tests.exe",
      L"phantombt-tests.exe",
      L"..\\..\\debug\\tests\\phantombt-tests.exe",
  };
  for (auto* c : kCandidates) {
    std::wstring full = dir + c;
    if (file_exists(full)) return pb::from_w(full);
  }
  return "";
}

Summary parse_summary(const std::string& output) {
  Summary s;
  // The framework prints exactly one summary line: "== 161/161 tests passed ==".
  // Anchor on the tail, then scan back to the opening "== ".
  size_t anchor = output.rfind("tests passed ==");
  if (anchor == std::string::npos) return s;
  size_t start = output.rfind("== ", anchor);
  if (start == std::string::npos) return s;
  int passed = 0, total = 0;
  if (sscanf_s(output.c_str() + start, "== %d/%d tests passed", &passed, &total) == 2 &&
      total > 0 && passed >= 0 && passed <= total) {
    s.ran = true;
    s.passed = passed;
    s.total = total;
  }
  return s;
}

int run_capture(const std::string& cmdline, const std::function<void(const std::string&)>& on_line,
                HANDLE stop, DWORD timeout_ms) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) return -1;
  // Child must not inherit the read end.
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"cmd.exe /c " + pb::to_w(cmdline);
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi);
  // Parent drops its copy of the write end so the pipe closes at child exit.
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(read_end);
    return -1;
  }

  // Poll instead of blocking in ReadFile: a blocking ReadFile can never be
  // cancelled (stop event / timeout) while the child holds the pipe open.
  // After the child exits, keep draining the pipe so late output written just
  // before exit is not lost.
  std::string pending;
  char buf[4096];
  bool timed_out = false, child_exited = false;
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    if (!child_exited) {
      HANDLE waits[2] = {pi.hProcess, stop ? stop : INVALID_HANDLE_VALUE};
      DWORD nw = stop ? 2 : 1;
      DWORD w = WaitForMultipleObjects(nw, waits, FALSE, 200);
      if (stop && w == WAIT_OBJECT_0 + 1) {  // caller-requested stop
        TerminateProcess(pi.hProcess, 1);
        timed_out = true;
        break;
      }
      if (w == WAIT_OBJECT_0) child_exited = true;  // switch to drain mode
      else if (GetTickCount64() >= deadline) {  // hard cap: kill a hung child
        TerminateProcess(pi.hProcess, 1);
        timed_out = true;
        break;
      }
    }
    DWORD avail = 0;
    if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr)) break;  // pipe gone
    if (avail == 0) {
      if (child_exited) break;  // writer closed and nothing left: drained
      continue;
    }
    DWORD n = 0;
    DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
    if (!ReadFile(read_end, buf, want, &n, nullptr) || n == 0) break;
    pending.append(buf, n);
    size_t start = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
      if (pending[i] == '\n') {
        std::string line = pending.substr(start, i - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (on_line) on_line(line);
        start = i + 1;
      }
    }
    pending.erase(0, start);
  }
  if (!timed_out && !pending.empty() && on_line) on_line(pending);
  CloseHandle(read_end);

  DWORD code = (DWORD)-1;
  if (timed_out) {
    // Give the terminated child a moment, then report timeout (-2) rather
    // than its meaningless kill exit code.
    WaitForSingleObject(pi.hProcess, 5000);
    code = (DWORD)-2;
  } else {
    WaitForSingleObject(pi.hProcess, 10000);
    if (!GetExitCodeProcess(pi.hProcess, &code) || code == STILL_ACTIVE) code = (DWORD)-2;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
}

}  // namespace pb::testrunner
