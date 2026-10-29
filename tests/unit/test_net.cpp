#include "test/test_framework.h"
#include "core/net.h"

#include <thread>

PB_TEST(net_loopback_send_recv) {
  pb::net::WsaSession wsa;
  pb::net::TcpListener l;
  std::string err;
  PB_CHECK(l.listen(0, &err));
  PB_CHECK(l.port() != 0);

  std::thread srv([&] {
    std::string e2;
    auto c = l.accept(&e2);
    PB_CHECK(c.is_open());
    uint8_t buf[5];
    PB_CHECK(c.recv_exact(buf, 5, &e2));
    const char reply[] = "pong!";
    c.send_all((const uint8_t*)reply, 5, &e2);
    c.close();
  });

  auto c = pb::net::TcpConn::connect("127.0.0.1", l.port(), &err);
  PB_CHECK(c.is_open());
  PB_CHECK(c.send_all((const uint8_t*)"ping!", 5, &err));
  uint8_t buf[5] = {};
  PB_CHECK(c.recv_exact(buf, 5, &err));
  PB_CHECK_EQ(std::string((char*)buf, 5), std::string("pong!"));
  srv.join();
}

PB_TEST(net_recv_exact_detects_close) {
  pb::net::WsaSession wsa;
  pb::net::TcpListener l;
  std::string err;
  PB_CHECK(l.listen(0, &err));
  std::thread srv([&] {
    std::string e2;
    auto c = l.accept(&e2);
    c.send_all((const uint8_t*)"ab", 2, &e2);
    c.close();  // 只发 2 字节就关闭
  });
  auto c = pb::net::TcpConn::connect("127.0.0.1", l.port(), &err);
  uint8_t buf[4];
  PB_CHECK(!c.recv_exact(buf, 4, &err));  // 4 字节永远等不到
  PB_CHECK(!err.empty());
  srv.join();
}

PB_TEST(net_connect_refused) {
  pb::net::WsaSession wsa;
  pb::net::TcpListener l;
  std::string err;
  PB_CHECK(l.listen(0, &err));
  uint16_t port = l.port();
  l.close();  // 关闭后再连 → 拒绝
  auto c = pb::net::TcpConn::connect("127.0.0.1", port, &err);
  PB_CHECK(!c.is_open());
  PB_CHECK(!err.empty());
}
