#pragma once
// 命名管道控制服务器（\\.\pipe\phantom-bt，行协议 = execute() 文本）+ 客户端
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sim/simulator.h"

namespace pb::control {

class PipeServer {
 public:
  explicit PipeServer(sim::Simulator& sim);
  ~PipeServer();
  bool start(std::string* err, const std::string& name = "\\\\.\\pipe\\phantom-bt");
  void stop();
  bool running() const { return running_.load(); }

 private:
  void client_loop(void* pipe_handle);
  sim::Simulator& sim_;
  std::atomic<bool> running_{false};
  std::vector<std::thread> threads_;
  std::string pipe_name_ = "\\\\.\\pipe\\phantom-bt";
};

// 管道客户端（GUI 客户端模式 / 远程 CLI）
class CommandClient {
 public:
  static bool send_line(const std::string& pipe_name, const std::string& line,
                        std::vector<std::string>& replies, int timeout_ms = 3000);
};

}  // namespace pb::control
