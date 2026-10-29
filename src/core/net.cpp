#include "core/net.h"

#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace pb::net {

namespace {
std::string wsa_err(const char* what) {
  int e = WSAGetLastError();
  return std::string(what) + " failed, WSAGetLastError=" + std::to_string(e);
}
}  // namespace

WsaSession::WsaSession() {
  WSADATA d;
  WSAStartup(MAKEWORD(2, 2), &d);
}
WsaSession::~WsaSession() { WSACleanup(); }

TcpConn::~TcpConn() { close(); }
TcpConn::TcpConn(TcpConn&& o) noexcept : s_(o.s_) { o.s_ = ~0ull; }
TcpConn& TcpConn::operator=(TcpConn&& o) noexcept {
  if (this != &o) {
    close();
    s_ = o.s_;
    o.s_ = ~0ull;
  }
  return *this;
}

TcpConn TcpConn::connect(const std::string& host, uint16_t port, std::string* err) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  std::string ps = std::to_string(port);
  if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &result) != 0 || !result) {
    if (err) *err = "getaddrinfo failed for " + host;
    return TcpConn{};
  }
  SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (s == INVALID_SOCKET) {
    if (err) *err = wsa_err("socket");
    freeaddrinfo(result);
    return TcpConn{};
  }
  if (::connect(s, result->ai_addr, (int)result->ai_addrlen) != 0) {
    if (err) *err = wsa_err("connect");
    closesocket(s);
    freeaddrinfo(result);
    return TcpConn{};
  }
  freeaddrinfo(result);
  TcpConn c = from_raw((uintptr_t)s);
  c.set_nodelay(true);
  return c;
}

TcpConn TcpConn::from_raw(uintptr_t s) {
  TcpConn c;
  c.s_ = s;
  return c;
}

bool TcpConn::send_all(const uint8_t* p, size_t n, std::string* err) {
  size_t sent = 0;
  while (sent < n) {
    int r = ::send((SOCKET)s_, (const char*)p + sent, (int)(n - sent), 0);
    if (r == SOCKET_ERROR) {
      if (err) *err = wsa_err("send");
      close();
      return false;
    }
    sent += (size_t)r;
  }
  return true;
}

bool TcpConn::recv_exact(uint8_t* p, size_t n, std::string* err) {
  size_t got = 0;
  while (got < n) {
    int r = ::recv((SOCKET)s_, (char*)p + got, (int)(n - got), 0);
    if (r == SOCKET_ERROR) {
      if (err) *err = wsa_err("recv");
      close();
      return false;
    }
    if (r == 0) {
      if (err) *err = "connection closed by peer";
      close();
      return false;
    }
    got += (size_t)r;
  }
  return true;
}

bool TcpConn::recv_some(uint8_t* buf, size_t cap, int* n_out, std::string* err) {
  int r = ::recv((SOCKET)s_, (char*)buf, (int)cap, 0);
  if (r == SOCKET_ERROR) {
    if (err) *err = wsa_err("recv");
    close();
    return false;
  }
  if (n_out) *n_out = r;  // 0 = 对端关闭
  return true;
}

void TcpConn::set_nodelay(bool on) {
  if (!is_open()) return;
  BOOL v = on ? TRUE : FALSE;
  setsockopt((SOCKET)s_, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, sizeof(v));
}

void TcpConn::close() {
  if (s_ != ~0ull) {
    ::shutdown((SOCKET)s_, SD_BOTH);
    closesocket((SOCKET)s_);
    s_ = ~0ull;
  }
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(uint16_t port, std::string* err) {
  s_ = (uintptr_t)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if ((SOCKET)s_ == INVALID_SOCKET) {
    if (err) *err = wsa_err("socket");
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt((SOCKET)s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind((SOCKET)s_, (sockaddr*)&addr, sizeof(addr)) != 0) {
    if (err) *err = wsa_err("bind");
    close();
    return false;
  }
  if (::listen((SOCKET)s_, 8) != 0) {
    if (err) *err = wsa_err("listen");
    close();
    return false;
  }
  sockaddr_in bound{};
  int len = sizeof(bound);
  getsockname((SOCKET)s_, (sockaddr*)&bound, &len);
  port_ = ntohs(bound.sin_port);
  return true;
}

TcpConn TcpListener::accept(std::string* err) {
  sockaddr_in peer{};
  int len = sizeof(peer);
  SOCKET c = ::accept((SOCKET)s_, (sockaddr*)&peer, &len);
  if (c == INVALID_SOCKET) {
    if (err) *err = wsa_err("accept");
    return TcpConn{};
  }
  TcpConn conn = TcpConn::from_raw((uintptr_t)c);
  conn.set_nodelay(true);
  return conn;
}

void TcpListener::close() {
  if (s_ != ~0ull) {
    closesocket((SOCKET)s_);
    s_ = ~0ull;
  }
}

}  // namespace pb::net
