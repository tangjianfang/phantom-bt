#include "test/test_framework.h"
#include "usbip/server.h"

#include <windows.h>

#include "core/bytes.h"
#include "core/net.h"
#include "hid/hid_model.h"
#include "hid/profiles.h"
#include "sim/device_registry.h"

using namespace pb;
using namespace pb::usbip;

static std::shared_ptr<hid::HidModel> add_vendor_device(sim::DeviceRegistry& reg) {
  auto prof = hid::HidProfile::from_json(json::parse(
      "{\"template\":\"vendor\",\"vid\":\"0x1209\",\"pid\":\"0xB00D\",\"product\":\"v\"}"));
  auto m = std::make_shared<hid::HidModel>(prof, 7, 1);
  reg.add(m);
  return m;
}

static void send_cmd(net::TcpConn& c, uint32_t seq, uint32_t ep, uint32_t dir, int32_t len,
                     const uint8_t setup[8], int32_t interval) {
  ByteWriter w;
  w.u32be(USBIP_CMD_SUBMIT);
  w.u32be(seq);
  w.u32be(0x00070001);
  w.u32be(dir);
  w.u32be(ep);
  w.u32be(0);
  w.u32be((uint32_t)len);
  w.u32be(0);
  w.u32be(0);
  w.u32be((uint32_t)interval);
  w.bytes(setup, 8);
  std::string err;
  if (!c.send_all(w.buf().data(), w.buf().size(), &err)) throw std::runtime_error("send_cmd: " + err);
}

static void send_raw(net::TcpConn& c, const std::vector<uint8_t>& v) {
  std::string err;
  if (!c.send_all(v.data(), v.size(), &err)) throw std::runtime_error("send_raw: " + err);
}

static std::vector<uint8_t> recv_n(net::TcpConn& c, size_t n) {
  std::vector<uint8_t> v(n);
  std::string err;
  if (!c.recv_exact(v.data(), n, &err)) throw std::runtime_error("recv_n: " + err);
  return v;
}

static void import_7_1(net::TcpConn& c) {
  ByteWriter w2;
  w2.u32be(OP_REQ_IMPORT);
  char busid[32] = "7-1";
  w2.bytes((const uint8_t*)busid, 32);
  send_raw(c, w2.buf());
  auto imp = recv_n(c, 8);
  if (imp[3] != 0x04 || imp[7] != 0x00) throw std::runtime_error("import_7_1: rejected");
  recv_n(c, 312);
}

PB_TEST(server_devlist_returns_device) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  add_vendor_device(reg);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());

  ByteWriter w1;
  w1.u32be(OP_REQ_DEVLIST);
  send_raw(c, w1.buf());
  auto head = recv_n(c, 12);
  PB_CHECK_EQ(head[3], 0x01);   // OP_REP_DEVLIST
  PB_CHECK_EQ(head[7], 0x00);   // status
  PB_CHECK_EQ(head[11], 0x01);  // ndev=1
  auto dev = recv_n(c, 312);
  PB_CHECK_EQ(dev[300], 0x12);  // vid BE @300
  auto iface = recv_n(c, 4);
  PB_CHECK_EQ(iface[0], 3);  // HID class
  c.close();
  srv.stop();
}

PB_TEST(server_import_and_control_get_descriptor) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  add_vendor_device(reg);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());

  import_7_1(c);

  uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  send_cmd(c, 1, 0, DIR_IN, 18, setup, 0);
  auto ret = recv_n(c, 40 + 18);
  PB_CHECK_EQ(ret[3], 0x03);   // RET_SUBMIT
  PB_CHECK_EQ(ret[7], 1);      // seqnum
  PB_CHECK_EQ(ret[40], 18);    // bLength
  PB_CHECK_EQ(ret[41], 1);     // DT_DEVICE
  PB_CHECK_EQ(ret[43], 0x02);  // bcdUSB hi

  uint8_t setup2[8] = {0x80, 6, 0, 2, 0, 0, 0xFF, 0};
  send_cmd(c, 2, 0, DIR_IN, 255, setup2, 0);
  auto ret2 = recv_n(c, 40 + 41);
  PB_CHECK_EQ(ret2[7], 2);
  PB_CHECK_EQ(ret2[42], 41);  // wTotalLength LE

  uint8_t setup3[8] = {0x80, 6, 2, 3, 9, 4, 0xFF, 0};
  send_cmd(c, 3, 0, DIR_IN, 255, setup3, 0);
  auto ret3 = recv_n(c, 40);
  size_t actual3 = ((size_t)ret3[24] << 24) | ((size_t)ret3[25] << 16) | ((size_t)ret3[26] << 8) | ret3[27];
  PB_CHECK_EQ(actual3, 4u);  // "v"（1 字符）→ 2 头 + 2
  auto sdata = recv_n(c, actual3);
  PB_CHECK_EQ(sdata[0], 4);
  PB_CHECK_EQ(sdata[1], 3);
  PB_CHECK_EQ(sdata[2], 'v');
  c.close();
  srv.stop();
}

PB_TEST(server_interrupt_in_pending_then_inject) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  auto model = add_vendor_device(reg);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());

  import_7_1(c);

  uint8_t zero8[8] = {};
  send_cmd(c, 10, 0x81, DIR_IN, 64, zero8, 10);
  Sleep(150);

  model->inject_input_report({9, 9});
  auto ret = recv_n(c, 40 + 2);
  PB_CHECK_EQ(ret[3], 0x03);
  PB_CHECK_EQ(ret[7], 10);
  PB_CHECK_EQ(ret[40], 9);
  PB_CHECK_EQ(ret[41], 9);

  model->inject_input_report({0xAB});
  send_cmd(c, 11, 0x81, DIR_IN, 64, zero8, 10);
  auto ret2 = recv_n(c, 40 + 1);
  PB_CHECK_EQ(ret2[7], 11);
  PB_CHECK_EQ(ret2[40], 0xAB);

  // unlink 挂起的 IN
  send_cmd(c, 12, 0x81, DIR_IN, 64, zero8, 10);
  Sleep(100);
  ByteWriter wu;
  wu.u32be(USBIP_CMD_UNLINK);
  wu.u32be(13);
  wu.u32be(0x00070001);
  wu.u32be(DIR_OUT);
  wu.u32be(0x81);
  wu.u32be(12);
  send_raw(c, wu.buf());
  auto ru = recv_n(c, 24);
  PB_CHECK_EQ(ru[3], 0x04);
  PB_CHECK_EQ(ru[7], 13);
  int32_t ust = (int32_t)(((uint32_t)ru[20] << 24) | ((uint32_t)ru[21] << 16) | ((uint32_t)ru[22] << 8) | ru[23]);
  PB_CHECK_EQ(ust, 0);
  c.close();
  srv.stop();
}

PB_TEST(server_import_unknown_busid_rejected) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  ByteWriter w2;
  w2.u32be(OP_REQ_IMPORT);
  char busid[32] = "9-9";
  w2.bytes((const uint8_t*)busid, 32);
  send_raw(c, w2.buf());
  auto imp = recv_n(c, 8);
  PB_CHECK_EQ(imp[3], 0x04);
  PB_CHECK_EQ(imp[7], 0x01);
  c.close();
  srv.stop();
}

PB_TEST(server_control_out_set_report_via_data_stage) {
  net::WsaSession w;
  sim::DeviceRegistry reg;
  auto model = add_vendor_device(reg);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  import_7_1(c);

  ByteWriter w3;
  w3.u32be(USBIP_CMD_SUBMIT);
  w3.u32be(20);
  w3.u32be(0x00070001);
  w3.u32be(DIR_OUT);
  w3.u32be(0);
  w3.u32be(0);
  w3.u32be(2);
  w3.u32be(0);
  w3.u32be(0);
  w3.u32be(0);
  uint8_t setup[8] = {0x21, 0x09, 0x00, 0x02, 0, 0, 2, 0};
  w3.bytes(setup, 8);
  w3.u8(0xEE);
  w3.u8(0xFF);
  send_raw(c, w3.buf());
  auto ret = recv_n(c, 40);
  PB_CHECK_EQ(ret[7], 20);
  auto rep = model->pop_output_report();
  PB_CHECK(rep.has_value());
  PB_CHECK_EQ((*rep)[0], 0xEE);
  PB_CHECK_EQ((*rep)[1], 0xFF);
  c.close();
  srv.stop();
}
