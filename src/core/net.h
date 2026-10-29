#pragma once
// Winsock TCP 封装（阻塞模式 + RAII）
#include <cstdint>
#include <optional>
#include <string>

namespace pb::net {

struct WsaSession {
  WsaSession();
  ~WsaSession();
};

class TcpConn {
 public:
  TcpConn() = default;
  ~TcpConn();
  TcpConn(TcpConn&& o) noexcept;
  TcpConn& operator=(TcpConn&& o) noexcept;
  TcpConn(const TcpConn&) = delete;
  TcpConn& operator=(const TcpConn&) = delete;

  static TcpConn connect(const std::string& host, uint16_t port, std::string* err);
  bool send_all(const uint8_t* p, size_t n, std::string* err);
  bool recv_exact(uint8_t* p, size_t n, std::string* err);
  bool recv_some(uint8_t* buf, size_t cap, int* n_out, std::string* err);
  void set_nodelay(bool on);
  void close();
  bool is_open() const { return s_ != INVALID_; }
  uintptr_t raw() const { return s_; }

  // 测试辅助：用已有 socket 构造（接管所有权）
  static TcpConn from_raw(uintptr_t s);

 private:
  static constexpr uintptr_t INVALID_ = ~0ull;
  uintptr_t s_ = INVALID_;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  bool listen(uint16_t port, std::string* err);
  TcpConn accept(std::string* err);
  uint16_t port() const { return port_; }
  void close();

 private:
  uintptr_t s_ = ~0ull;
  uint16_t port_ = 0;
};

}  // namespace pb::net
