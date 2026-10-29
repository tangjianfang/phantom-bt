#include "core/test_runner.h"

#include <windows.h>

#include <string>
#include <vector>

#include "core/bytes.h"
#include "core/env_check.h"

namespace pb::testrunner {

namespace {

bool file_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Best-effort tree kill: the job takes grandchildren, TerminateProcess is the
// unconditional fallback for the main child (never left running).
void kill_tree(HANDLE job, bool job_ok, HANDLE process) {
  if (job_ok) TerminateJobObject(job, 1);
  TerminateProcess(process, 1);
}

}  // namespace

std::string find_tests_exe() {
  // Layout produced by the presets: build/<cfg>/src/phantombt-gui.exe and
  // build/<cfg>/tests/phantombt-tests.exe, so from the GUI exe it is
  // ..\tests\phantombt-tests.exe. Also try the same dir and the debug tree.
  std::wstring dir = pb::env::exe_dir();
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

  // Kill the whole process tree if this process dies or the job handle closes.
  // Every step is checked: if the job cannot be created/configured/assigned
  // (quota, hostile nested-job policy), fall back to killing the main child
  // directly so a stop/timeout can never leave it running.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  bool job_ok = job != nullptr;
  if (job_ok) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    job_ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl)) != 0;
  }

  // Launch directly — no cmd.exe wrapper. cmd /c would strip the outer quotes
  // (breaking paths with spaces) and put the real target in a grandchild
  // process that TerminateProcess on cmd can never reach.
  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = pb::to_w(cmdline);
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
  // Parent drops its copy of the write end so the pipe closes at child exit.
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(read_end);
    if (job) CloseHandle(job);
    return -1;
  }
  if (job_ok) job_ok = AssignProcessToJobObject(job, pi.hProcess) != 0;  // still suspended
  ResumeThread(pi.hThread);

  // Poll instead of blocking in ReadFile: a blocking ReadFile can never be
  // cancelled (stop event / timeout) while the child holds the pipe open.
  // Drain greedily in an inner loop (one 4KB read per 200ms wait would cap
  // throughput at ~20KB/s and time out healthy chatty runs), and after the
  // child exits keep reading for a short grace window so output from a
  // grandchild still holding the write end is not dropped.
  std::string pending;
  char buf[8192];
  bool timed_out = false, child_exited = false, busy = false;
  int quiet_polls = 0;
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    // Stop event and hard timeout are checked in EVERY mode — including the
    // post-exit drain loop, or a chatty grandchild holding the write end
    // would keep run_capture (and the GUI's shutdown join) stuck forever.
    if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
      if (!pending.empty() && on_line) on_line(pending);  // keep the killed tail
      kill_tree(job, job_ok, pi.hProcess);
      timed_out = true;
      break;
    }
    if (GetTickCount64() >= deadline) {  // hard cap (hung child or endless drain)
      if (!pending.empty() && on_line) on_line(pending);
      kill_tree(job, job_ok, pi.hProcess);
      timed_out = true;
      break;
    }
    if (!child_exited) {
      // Data does not wake the wait: poll slowly only when idle, and nearly
      // busy-poll while the child is producing, or throughput is capped at
      // one pipe-buffer per wait interval.
      HANDLE waits[2] = {pi.hProcess, stop ? stop : INVALID_HANDLE_VALUE};
      DWORD nw = stop ? 2 : 1;
      DWORD w = WaitForMultipleObjects(nw, waits, FALSE, busy ? 1 : 200);
      if (w == WAIT_OBJECT_0) child_exited = true;  // switch to drain mode
    } else if (quiet_polls > 0) {
      Sleep(50);  // grace window: a grandchild may still be writing
    }
    bool got_any = false;
    for (;;) {  // drain everything currently available
      DWORD avail = 0;
      if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr)) goto pipe_gone;
      if (avail == 0) break;
      DWORD n = 0;
      DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
      if (!ReadFile(read_end, buf, want, &n, nullptr) || n == 0) goto pipe_gone;
      got_any = true;
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
    if (child_exited) {
      if (got_any) quiet_polls = 0;
      else if (++quiet_polls >= 6) {  // ~300ms silent after exit: drained
        if (!pending.empty() && on_line) on_line(pending);
        break;
      }
    }
    busy = got_any;  // fast poll next round iff we just moved data
    continue;
  pipe_gone:
    if (!pending.empty() && on_line && !timed_out) on_line(pending);
    pending.clear();
    break;
  }
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
  if (job) CloseHandle(job);  // kills any grandchildren (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
  return (int)code;
}

}  // namespace pb::testrunner
