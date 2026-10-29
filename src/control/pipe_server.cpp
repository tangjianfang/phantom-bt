#include "control/pipe_server.h"

#include <atomic>
#include <thread>

#include <windows.h>

#include "control/commands.h"
#include "core/log.h"

namespace pb::control {

PipeServer::PipeServer(sim::Simulator& sim) : sim_(sim) {}
PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(std::string* err, const std::string& name) {
  // 单实例监听管道：每次连接断开后重建（简化模型）
  running_ = true;
  pipe_name_ = name;
  threads_.emplace_back([this, name] {
    while (running_.load()) {
      HANDLE pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                     1, 4096, 4096, 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) {
        PB_LOG_WARN("pipe: CreateNamedPipe failed " + std::to_string(GetLastError()));
        Sleep(200);
        continue;
      }
      BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
      if (!connected || !running_.load()) {
        CloseHandle(pipe);
        continue;
      }
      client_loop(pipe);
      FlushFileBuffers(pipe);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
  });
  PB_LOG_INFO("pipe: control server on " + name);
  return true;
}

void PipeServer::stop() {
  if (!running_.exchange(false)) return;
  // 唤醒监听线程：与其 CreateNamedPipe 的间隙竞态 → 短重试确保送达
  for (int i = 0; i < 20; ++i) {
    HANDLE wake = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
      CloseHandle(wake);
      break;
    }
    Sleep(100);
  }
  for (auto& t : threads_)
    if (t.joinable()) t.join();
  threads_.clear();
}

void PipeServer::client_loop(void* pipe_handle) {
  HANDLE pipe = (HANDLE)pipe_handle;
  std::string line;
  char buf[512];
  DWORD n = 0;
  while (running_.load()) {
    if (!ReadFile(pipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
    line.append(buf, buf + n);
    size_t pos;
    while ((pos = line.find('\n')) != std::string::npos) {
      std::string cmd = line.substr(0, pos);
      line.erase(0, pos + 1);
      while (!cmd.empty() && (cmd.back() == '\r')) cmd.pop_back();
      if (cmd.empty()) continue;
      auto replies = execute(sim_, cmd);
      std::string out;
      for (auto& r : replies) out += r + "\n";
      DWORD written = 0;
      WriteFile(pipe, out.data(), (DWORD)out.size(), &written, nullptr);
    }
  }
}

bool CommandClient::send_line(const std::string& pipe_name, const std::string& line,
                              std::vector<std::string>& replies, int timeout_ms) {
  // 服务器线程可能尚未创建管道实例：短重试窗口
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 40 && pipe == INVALID_HANDLE_VALUE; ++i) {
    pipe = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) Sleep(25);
  }
  if (pipe == INVALID_HANDLE_VALUE) return false;
  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  std::string out = line + "\n";
  DWORD written = 0;
  if (!WriteFile(pipe, out.data(), (DWORD)out.size(), &written, nullptr)) {
    CloseHandle(pipe);
    return false;
  }
  // 读取到空闲（简化：读到 100ms 无数据或行结尾 OK/ERR）
  std::string acc;
  char buf[1024];
  DWORD n = 0;
  auto deadline = GetTickCount64() + timeout_ms;
  while (GetTickCount64() < deadline) {
    if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
      if (!ReadFile(pipe, buf, std::min<DWORD>(n, sizeof(buf)), &n, nullptr)) break;
      acc.append(buf, buf + n);
      // 完整行集合以 \n 结尾即视为一批应答
      if (!acc.empty() && acc.back() == '\n') break;
    } else {
      Sleep(20);
    }
  }
  CloseHandle(pipe);
  size_t start = 0;
  while (start < acc.size()) {
    size_t e = acc.find('\n', start);
    if (e == std::string::npos) break;
    std::string l = acc.substr(start, e - start);
    if (!l.empty()) replies.push_back(l);
    start = e + 1;
  }
  return !replies.empty();
}

}  // namespace pb::control
