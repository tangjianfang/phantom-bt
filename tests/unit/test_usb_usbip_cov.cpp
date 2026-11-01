// 覆盖率补充（R1）：src/usb/control.cpp / src/usb/control.h / src/usb/descriptors.cpp /
// src/usbip/server.cpp 的未覆盖行。
// 服务器测试全部走 127.0.0.1 回环 + 事件标志（atomic）同步，无 Sleep、无时序假设；
// 同一连接上的请求按 TCP 有序处理，因此“先提交后 unlink”等顺序无需等待。
#include "test/test_framework.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/bytes.h"
#include "core/net.h"
#include "sim/device_registry.h"
#include "usb/control.h"
#include "usb/descriptors.h"
#include "usbip/protocol.h"
#include "usbip/server.h"

using namespace pb;
using namespace pb::usbip;

// ============ usb/control.cpp：GET_CONFIGURATION / SET_INTERFACE / default / 字符串命中 ============

namespace {
usb::ControlSetup cov_setup(uint8_t bm, uint8_t br, uint16_t wvalue, uint16_t wlength) {
  const uint8_t s[8] = {bm, br, (uint8_t)(wvalue & 0xFF), (uint8_t)(wvalue >> 8), 0, 0,
                        (uint8_t)(wlength & 0xFF), (uint8_t)(wlength >> 8)};
  return usb::parse_setup(s);
}
}  // namespace

PB_TEST(cov_usb_usbip_control_get_configuration_wlength) {
  usb::DescriptorStore s;
  s.device = usb::build_device_descriptor(usb::ConfigParams{});
  std::vector<uint8_t> out;
  // wLength=1：返回 {1}
  PB_CHECK(usb::descriptor_request(cov_setup(0x80, usb::REQ_GET_CONFIGURATION, 0, 1), s, out));
  PB_CHECK_EQ(out.size(), 1u);
  PB_CHECK_EQ(out[0], 1u);
  // wLength=0：截断为空
  PB_CHECK(usb::descriptor_request(cov_setup(0x80, usb::REQ_GET_CONFIGURATION, 0, 0), s, out));
  PB_CHECK(out.empty());
}

PB_TEST(cov_usb_usbip_control_set_interface_unknown_and_langid) {
  usb::DescriptorStore s;
  s.strings[0] = usb::build_string_descriptor_zero();
  std::vector<uint8_t> out;
  // SET_INTERFACE：空应答
  PB_CHECK(usb::descriptor_request(cov_setup(0x01, usb::REQ_SET_INTERFACE, 0, 0), s, out));
  PB_CHECK(out.empty());
  // 未知标准请求（GET_INTERFACE 未实现）→ default 分支
  PB_CHECK(!usb::descriptor_request(cov_setup(0x81, usb::REQ_GET_INTERFACE, 0, 1), s, out));
  // 字符串 index 0（LANGID）命中 DT_STRING 找到路径
  PB_CHECK(usb::descriptor_request(cov_setup(0x80, usb::REQ_GET_DESCRIPTOR, 0x0300, 4), s, out));
  PB_CHECK_EQ(out.size(), 4u);
}

// ============ usb/control.h:73：IUsbDeviceModel::on_stall_clear 默认实现 ============

namespace {
struct CovBareModel : usb::IUsbDeviceModel {
  usbip::ExportedDevice dev_;
  const usbip::ExportedDevice& exported() const override { return dev_; }
  void attach_sink(usb::IUsbModelSink*) override {}
  std::vector<uint8_t> on_control(const usb::ControlSetup&, const std::vector<uint8_t>&) override {
    return {};
  }
  void on_out(uint8_t, std::vector<uint8_t>) override {}
  void on_in(uint8_t) override {}
  void on_detach() override {}
};
}  // namespace

PB_TEST(cov_usb_usbip_model_default_on_stall_clear) {
  CovBareModel m;  // 不覆写 on_stall_clear → 执行基类默认空实现（control.h:73）
  usb::IUsbDeviceModel& base = m;
  base.on_stall_clear(0x81);
  PB_CHECK(true);
}

// ============ usb/descriptors.cpp：IAD 块 / 多字节 UTF-8 / 255 截断 ============

PB_TEST(cov_usb_usbip_desc_config_with_iad) {
  usb::ConfigParams c;
  c.vid = 1;
  c.pid = 2;
  c.iads = {{0, 2, 0xE0, 0x01, 0x01, 0}};  // 首接口 0，聚合 2 个接口
  usb::InterfaceDef i0;
  i0.number = 0;
  i0.iclass = 3;
  usb::InterfaceDef i1;
  i1.number = 1;
  i1.iclass = 0xE0;
  c.interfaces = {i0, i1};
  auto cfg = usb::build_config_descriptor(c);
  // 9(config) + [8(IAD)+9(iface0)] + 9(iface1，IAD 不匹配) = 35
  PB_CHECK_EQ(cfg.size(), 35u);
  PB_CHECK_EQ(cfg[2], 35u);  // wTotalLength LE
  PB_CHECK_EQ(cfg[9], 8u);   // IAD bLength
  PB_CHECK_EQ(cfg[10], 0x0Bu);
  PB_CHECK_EQ(cfg[11], 0u);  // bFirstInterface
  PB_CHECK_EQ(cfg[12], 2u);  // bInterfaceCount
  PB_CHECK_EQ(cfg[13], 0xE0u);
  PB_CHECK_EQ(cfg[14], 0x01u);
  PB_CHECK_EQ(cfg[15], 0x01u);
  PB_CHECK_EQ(cfg[16], 0u);  // iFunction
  PB_CHECK_EQ(cfg[17], 9u);  // iface0 bLength
  PB_CHECK_EQ(cfg[18], 4u);
  PB_CHECK_EQ(cfg[19], 0u);
  PB_CHECK_EQ(cfg[22], 3u);  // iface0 class
  PB_CHECK_EQ(cfg[26], 9u);  // iface1 bLength（IAD 循环对 number=1 不命中）
  PB_CHECK_EQ(cfg[28], 1u);
}

PB_TEST(cov_usb_usbip_desc_string_multibyte_utf8) {
  // 2 字节序列（é = C3 A9 → U+00E9）
  auto s2 = usb::build_string_descriptor("\xC3\xA9");
  PB_CHECK_EQ(s2.size(), 4u);
  PB_CHECK_EQ(s2[0], 4u);
  PB_CHECK_EQ(s2[1], 3u);
  PB_CHECK_EQ(s2[2], 0xE9u);
  PB_CHECK_EQ(s2[3], 0x00u);
  // 4 字节序列（😀 = F0 9F 98 80 → U+1F600 → 代理对 D83D DE00）
  auto s4 = usb::build_string_descriptor("\xF0\x9F\x98\x80");
  PB_CHECK_EQ(s4.size(), 6u);
  PB_CHECK_EQ(s4[0], 6u);
  PB_CHECK_EQ(s4[2], 0x3Du);
  PB_CHECK_EQ(s4[3], 0xD8u);
  PB_CHECK_EQ(s4[4], 0x00u);
  PB_CHECK_EQ(s4[5], 0xDEu);
}

PB_TEST(cov_usb_usbip_desc_string_truncated_at_255) {
  // 127 个 ASCII → body 254B + 头 2B = 256 > 255 → 截到 253/255
  auto s = usb::build_string_descriptor(std::string(127, 'A'));
  PB_CHECK_EQ(s.size(), 255u);
  PB_CHECK_EQ(s[0], 255u);
  PB_CHECK_EQ(s[1], 3u);
  PB_CHECK_EQ(s[2], 'A');
  PB_CHECK_EQ(s[254], 'A');
}

// ============ usbip/server.cpp：可控模型 + 回环 TCP ============

namespace {

// 可编程测试模型：按标志在 URB 回调里触发 STALL / complete_in / stall_ep / 阻塞
class CovModel : public usb::IUsbDeviceModel {
 public:
  explicit CovModel(const char* busid) {
    dev_.path = std::string("/phantom/") + busid;
    dev_.busid = busid;
  }
  const usbip::ExportedDevice& exported() const override { return dev_; }
  void attach_sink(usb::IUsbModelSink* s) override { sink = s; }

  std::vector<uint8_t> on_control(const usb::ControlSetup& s, const std::vector<uint8_t>&) override {
    ++control_calls;
    if (block_mode.load()) {
      entered.store(true);
      while (!release.load()) std::this_thread::yield();
      ++resumed;
    }
    if (s.bRequest == 0x66) throw usb::ControlStall("cov stall");
    return {0x12, 0x01};
  }
  void on_out(uint8_t ep, std::vector<uint8_t> data) override {
    last_out_ep = ep;
    last_out_data = std::move(data);
    ++out_calls;
    if (sink) {
      if (complete_no_pending.load()) sink->complete_in(0x89, 0, {0xAA});  // 无挂起 IN → 早退分支
      if (stall_no_pending.load()) sink->stall_ep(0x88);                  // 无挂起 IN → 早退分支
    }
  }
  void on_in(uint8_t ep) override {
    ++in_calls;
    if (stall_on_in.load() && sink) sink->stall_ep(ep);  // 有挂起 IN → RET_SUBMIT EPIPE
  }
  void on_detach() override { ++detach_calls; }

  usbip::ExportedDevice dev_;
  usb::IUsbModelSink* sink = nullptr;
  std::atomic<bool> block_mode{false}, entered{false}, release{false};
  std::atomic<bool> stall_on_in{false}, complete_no_pending{false}, stall_no_pending{false};
  std::atomic<int> control_calls{0}, resumed{0}, out_calls{0}, in_calls{0}, detach_calls{0};
  uint8_t last_out_ep = 0;
  std::vector<uint8_t> last_out_data;
};

void cov_send_raw(net::TcpConn& c, const std::vector<uint8_t>& v) {
  std::string err;
  if (!c.send_all(v.data(), v.size(), &err)) throw std::runtime_error("cov send: " + err);
}

std::vector<uint8_t> cov_recv_n(net::TcpConn& c, size_t n) {
  std::vector<uint8_t> v(n);
  std::string err;
  if (!c.recv_exact(v.data(), n, &err)) throw std::runtime_error("cov recv: " + err);
  return v;
}

// 旧协议遗留提示：op 阶段已迁移为 op_common 8B（u16 version=0x0111 + u16 code +
// u32 status），IMPORT 请求 = 8B 头 + 32B busid；应答 REP_IMPORT(status=0) + 312B 设备。
void cov_import(net::TcpConn& c, const char* busid) {
  ByteWriter w;
  w.u16be(OP_VERSION);
  w.u16be(OP_REQ_IMPORT);
  w.u32be(0);
  char b[32] = {};
  for (size_t i = 0; i < 32 && busid[i]; ++i) b[i] = busid[i];
  w.bytes((const uint8_t*)b, 32);
  cov_send_raw(c, w.buf());
  auto imp = cov_recv_n(c, 8);
  if (imp[1] != 0x11 || imp[3] != 0x03 || imp[7] != 0x00)
    throw std::runtime_error("cov import rejected");
  cov_recv_n(c, USB_DEVICE_PACKED_SIZE);
}

void cov_send_submit(net::TcpConn& c, uint32_t seq, uint32_t ep, uint32_t dir, int32_t tbl,
                     const uint8_t setup[8], const std::vector<uint8_t>& payload = {}) {
  ByteWriter w;
  w.u32be(USBIP_CMD_SUBMIT);
  w.u32be(seq);
  w.u32be(0x00070009u);  // devid：服务器仅回显
  w.u32be(dir);
  w.u32be(ep);
  w.u32be(0);  // transfer_flags
  w.u32be((uint32_t)tbl);
  w.u32be(0);  // start_frame
  w.u32be(0);  // number_of_packets
  w.u32be(0);  // interval
  w.bytes(setup, 8);
  if (!payload.empty()) w.bytes(payload.data(), payload.size());
  cov_send_raw(c, w.buf());
}

void cov_send_unlink(net::TcpConn& c, uint32_t seq, uint32_t ep, uint32_t unlink_seqnum) {
  ByteWriter w;
  w.u32be(USBIP_CMD_UNLINK);
  w.u32be(seq);
  w.u32be(0x00070009u);
  w.u32be(DIR_OUT);
  w.u32be(ep);
  w.u32be(unlink_seqnum);
  cov_send_raw(c, w.buf());
}

int32_t cov_be32(const std::vector<uint8_t>& v, size_t off) {
  return (int32_t)(((uint32_t)v[off] << 24) | ((uint32_t)v[off + 1] << 16) |
                   ((uint32_t)v[off + 2] << 8) | v[off + 3]);
}

// 事件等待（非时序）：上限只作为失败保护，正常路径由事件标志触发
bool cov_spin_until(const std::atomic<bool>& f) {
  for (int i = 0; i < 5000000; ++i) {
    if (f.load()) return true;
    std::this_thread::yield();
  }
  return f.load();
}

}  // namespace

// OP_REQ_IMPORT 携带全零 busid → parse_import_busid 失败 → REP_IMPORT status=1，连接保持
PB_TEST(cov_usb_usbip_server_import_empty_busid_rejected) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());

  ByteWriter w;
  w.u16be(OP_VERSION);
  w.u16be(OP_REQ_IMPORT);
  w.u32be(0);
  uint8_t zeros[32] = {};
  w.bytes(zeros, 32);
  cov_send_raw(c, w.buf());
  auto imp = cov_recv_n(c, 8);
  PB_CHECK_EQ(imp[1], 0x11);
  PB_CHECK_EQ(imp[3], 0x03);  // OP_REP_IMPORT
  PB_CHECK_EQ(imp[7], 0x01);  // status = error

  // 控制循环继续：随后 DEVLIST 仍应答
  ByteWriter w2;
  w2.u16be(OP_VERSION);
  w2.u16be(OP_REQ_DEVLIST);
  w2.u32be(0);
  cov_send_raw(c, w2.buf());
  auto head = cov_recv_n(c, 12);
  PB_CHECK_EQ(head[3], 0x05);  // OP_REP_DEVLIST
  PB_CHECK_EQ(head[7], 0x00);
  PB_CHECK_EQ(head[11], 0x00);  // ndev = 0
  c.close();
  srv.stop();
}

// 未知控制 opcode → 服务器直接断开（客户端读到 EOF）
PB_TEST(cov_usb_usbip_server_unknown_opcode_disconnects) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  // 必须发满完整 8B op_common 头：服务器按 8B 定长读取，只发 4B 会让双方
  // 互相等待（recv 对 recv 死锁 → 套件挂死）。code 取未定义值即可。
  ByteWriter w;
  w.u16be(OP_VERSION);
  w.u16be(0x0007);  // 未定义 op code
  w.u32be(0);
  cov_send_raw(c, w.buf());
  uint8_t b = 0;
  std::string e2;
  PB_CHECK(!c.recv_exact(&b, 1, &e2));  // 服务器 break 后关闭连接
  c.close();
  srv.stop();
}

// EP0 控制传输：模型抛 ControlStall → RET_SUBMIT status=EPIPE(-32)、actual=0
PB_TEST(cov_usb_usbip_server_ep0_control_stall) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-3");
  PB_CHECK_EQ(reg.add(model), std::string("7-3"));  // registry 沿用模型自带 busid
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-3");

  uint8_t setup[8] = {0x80, 0x66, 0, 0, 0, 0, 2, 0};
  cov_send_submit(c, 7, 0, DIR_IN, 2, setup);
  auto ret = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret[3], 0x03);
  PB_CHECK_EQ(ret[7], 7);
  PB_CHECK_EQ(cov_be32(ret, 20), URB_STATUS_EPIPE);
  PB_CHECK_EQ(cov_be32(ret, 24), 0);
  PB_CHECK_EQ(model->control_calls.load(), 1);
  c.close();
  srv.stop();
}

// 非 EP0 的 OUT 传输（bulk OUT）：读数据阶段 → 模型 on_out → RET_SUBMIT actual=tbl
PB_TEST(cov_usb_usbip_server_bulk_out_to_model) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-4");
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-4");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 30, 2, DIR_OUT, 3, zero8, {1, 2, 3});
  auto ret = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret[7], 30);
  PB_CHECK_EQ(cov_be32(ret, 20), 0);
  PB_CHECK_EQ(cov_be32(ret, 24), 3);
  PB_CHECK_EQ(model->out_calls.load(), 1);
  PB_CHECK_EQ(model->last_out_ep, 2);
  PB_CHECK_EQ(model->last_out_data, (std::vector<uint8_t>{1, 2, 3}));

  // tbl=0：跳过数据阶段，on_out 收到空 data
  cov_send_submit(c, 31, 2, DIR_OUT, 0, zero8);
  auto ret2 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret2[7], 31);
  PB_CHECK_EQ(cov_be32(ret2, 24), 0);
  PB_CHECK_EQ(model->out_calls.load(), 2);
  PB_CHECK(model->last_out_data.empty());
  c.close();
  srv.stop();
}

// 挂起 IN 的 STALL 应答 + complete_in/stall_ep 无挂起 URB 时的早退分支
PB_TEST(cov_usb_usbip_server_in_stall_and_sink_early_returns) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-5");
  model->stall_on_in = true;
  model->complete_no_pending = true;
  model->stall_no_pending = true;
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-5");

  // IN 提交 → 服务器登记 pending_ins[0x81] → on_in → stall_ep 命中挂起 → RET_SUBMIT EPIPE
  uint8_t zero8[8] = {};
  cov_send_submit(c, 21, 1, DIR_IN, 4, zero8);
  auto ret = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret[7], 21);
  PB_CHECK_EQ(ret[16], 0x00);
  PB_CHECK_EQ(ret[19], 0x81);  // ep 回显（含方向位）
  PB_CHECK_EQ(cov_be32(ret, 20), URB_STATUS_EPIPE);
  PB_CHECK_EQ(cov_be32(ret, 24), 0);
  PB_CHECK_EQ(model->in_calls.load(), 1);

  // OUT 触发模型对无挂起端点调 complete_in(0x89)/stall_ep(0x88)：均早退、不产生额外应答
  cov_send_submit(c, 22, 2, DIR_OUT, 2, zero8, {0xEE, 0xFF});
  auto ret2 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret2[7], 22);
  PB_CHECK_EQ(cov_be32(ret2, 20), 0);
  PB_CHECK_EQ(model->out_calls.load(), 1);
  c.close();
  srv.stop();
}

// UNLINK 扫描：先失配（status=-1），再命中第二个挂起端点（status=0）
PB_TEST(cov_usb_usbip_server_unlink_scan_miss_then_match) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-6");
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-6");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 10, 1, DIR_IN, 64, zero8);  // pending[0x81]=seq10
  cov_send_submit(c, 11, 2, DIR_IN, 64, zero8);  // pending[0x82]=seq11
  // 同一 TCP 流有序：两条提交必先于 unlink 入队处理，无需等待
  cov_send_unlink(c, 12, 0x81, 99);              // 扫描全部失配 → status=-1
  auto ru = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ(ru[3], 0x04);
  PB_CHECK_EQ(ru[7], 12);
  PB_CHECK_EQ(cov_be32(ru, 20), -1);

  cov_send_unlink(c, 13, 0x82, 11);  // 第一项(0x81/seq10)失配后命中 0x82/seq11 → status=0
  auto ru2 = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ(ru2[7], 13);
  PB_CHECK_EQ(cov_be32(ru2, 20), 0);
  PB_CHECK_EQ(model->in_calls.load(), 2);
  c.close();
  srv.stop();
}

// URB 循环内未知 opcode → 断开并触发模型 on_detach
PB_TEST(cov_usb_usbip_server_unknown_urb_opcode_disconnects) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-7");
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-7");

  ByteWriter w;
  w.u32be(0x00000009u);  // 未知 URB opcode（完整 20B 基本头，服务器读满即判未知并断开）
  w.u32be(1);            // seq
  w.u32be(0x00070009u);  // devid
  w.u32be(DIR_OUT);      // direction
  w.u32be(0);            // ep
  cov_send_raw(c, w.buf());
  uint8_t b = 0;
  std::string e2;
  PB_CHECK(!c.recv_exact(&b, 1, &e2));           // 服务器 return false → 清理 → 关闭
  PB_CHECK_EQ(model->detach_calls.load(), 1);    // 关闭前已回调 on_detach
  c.close();
  srv.stop();
}

// stop() 与挂起 URB 竞态的确定性构造：模型 on_control 阻塞 → stop() 关闭连接并置
// running=false → 放行后 reply_submit 的 send 失败（send_locked 失败分支），URB 循环
// 因 running=false 退出（尾部 return false）。全程事件定序，无时序假设。
PB_TEST(cov_usb_usbip_server_stop_during_urb_send_fails) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-8");
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-8");

  // 客户端读线程：连接被服务器（stop）关闭时收到 EOF
  std::atomic<bool> reader_done{false}, reader_got_data{false};
  std::thread reader([&] {
    uint8_t buf[42];
    std::string e;
    bool ok = c.recv_exact(buf, 42, &e);
    reader_got_data.store(ok);
    reader_done.store(true);
  });

  model->block_mode.store(true);
  uint8_t setup[8] = {0x80, 0x06, 0, 1, 0, 0, 2, 0};
  cov_send_submit(c, 1, 0, DIR_IN, 2, setup);
  PB_CHECK(cov_spin_until(model->entered));  // 服务器线程已在 on_control 内阻塞

  std::thread stopper([&] { srv.stop(); });  // running=false → 关闭服务端 socket → join 连接线程
  PB_CHECK(cov_spin_until(reader_done));     // 服务端 socket 已关闭（stop 的关闭阶段先于 join）
  model->release.store(true);                // 放行：send 必失败（socket 已失效），随后循环条件为假

  stopper.join();
  reader.join();
  PB_CHECK(!reader_got_data.load());             // 应答未能发出（send 失败）
  PB_CHECK_EQ(model->resumed.load(), 1);         // on_control 已返回并走完 reply_submit
  PB_CHECK_EQ(model->detach_calls.load(), 1);    // URB 循环退出后进入清理
  srv.stop();                                    // 幂等：running 已为 false
}

// ============ R1 追加（第 2 批）：op_common 8B 头线协议下的服务器路径 ============
// src/usbip 协议已迁移：op 阶段为 op_common 8B（u16 version=0x0111 + u16 code + u32
// status），REP/RET 帧按 usbip_header 定长（RET_SUBMIT/RET_UNLINK 48B）。下列请求均按
// 当前协议编码。同步全部走回环 TCP 请求/应答 + atomic 事件标志，无 Sleep、无时序假设。

namespace {

// 发送 op_common 请求：8B 头 + 可选请求体（IMPORT 的 32B busid）
void cov2_send_op(net::TcpConn& c, uint16_t code, const void* body = nullptr, size_t body_len = 0) {
  ByteWriter w;
  w.u16be(OP_VERSION);
  w.u16be(code);
  w.u32be(0);  // status：请求恒 0
  if (body) w.bytes((const uint8_t*)body, body_len);
  cov_send_raw(c, w.buf());
}

// 新协议 import：应答 = REP_IMPORT(8B, status=0) + usbip_usb_device(312B)
void cov2_import(net::TcpConn& c, const char* busid) {
  char b[32] = {};
  for (size_t i = 0; i < 32 && busid[i]; ++i) b[i] = busid[i];
  cov2_send_op(c, OP_REQ_IMPORT, b, sizeof(b));
  auto imp = cov_recv_n(c, 8);
  if (imp[0] != 0x01 || imp[1] != 0x11 || imp[3] != (OP_REP_IMPORT & 0xFF) || imp[7] != 0x00)
    throw std::runtime_error("cov2 import rejected");
  cov_recv_n(c, USB_DEVICE_PACKED_SIZE);
}

// 有界事件等待（计数版）：正常路径由事件触发，上限仅作失败保护
bool cov2_spin_cnt(const std::atomic<int>& c, int want) {
  for (int i = 0; i < 5000000; ++i) {
    if (c.load() >= want) return true;
    std::this_thread::yield();
  }
  return c.load() >= want;
}

// 在 CovModel 基础上记录 EP0 控制传输 data 阶段（写入后递增 ctrl_done 建立 happens-before）
struct CovV2Model : CovModel {
  explicit CovV2Model(const char* busid) : CovModel(busid) {}
  std::vector<uint8_t> on_control(const usb::ControlSetup& s, const std::vector<uint8_t>& data) override {
    auto r = CovModel::on_control(s, data);
    last_ctrl_data = data;
    ++ctrl_done;
    return r;
  }
  std::vector<uint8_t> last_ctrl_data;
  std::atomic<int> ctrl_done{0};
};

}  // namespace

// OP_REQ_DEVLIST：应答 op_common 头（version 0x0111 / code 0x0005 / status 0）+ ndev +
// usbip_usb_device；continue 后控制循环继续服务第二次 DEVLIST（server.cpp:167-172）
PB_TEST(cov_usb_usbip_v2_devlist_reply_continues) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-11");
  PB_CHECK_EQ(reg.add(model), std::string("7-11"));  // registry 沿用模型自带 busid
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());

  cov2_send_op(c, OP_REQ_DEVLIST);
  auto head = cov_recv_n(c, 8);
  PB_CHECK_EQ(head[0], 0x01);                 // version hi
  PB_CHECK_EQ(head[1], 0x11);                 // version lo
  PB_CHECK_EQ(head[3], OP_REP_DEVLIST & 0xFF);  // code = 0x0005
  PB_CHECK_EQ(head[7], 0x00);                 // status = 0
  auto ndev = cov_recv_n(c, 4);
  PB_CHECK_EQ(ndev[3], 1);
  auto dev = cov_recv_n(c, USB_DEVICE_PACKED_SIZE);
  PB_CHECK_EQ(dev[0], '/');  // path "/phantom/7-11"
  PB_CHECK_EQ(dev[256], '7');  // busid 字段 @256 = "7-11"
  PB_CHECK_EQ(dev[258], '1');

  // 第二次 DEVLIST 仍应答：证明 continue 分支后控制循环存活
  cov2_send_op(c, OP_REQ_DEVLIST);
  auto head2 = cov_recv_n(c, 8);
  PB_CHECK_EQ(head2[3], OP_REP_DEVLIST & 0xFF);
  cov_recv_n(c, 4 + USB_DEVICE_PACKED_SIZE);
  c.close();
  srv.stop();  // join 连接线程 → 自然退出清理路径已执行
}

// op_common version != 0x0111 → 警告并断开（server.cpp:162-164），客户端读到 EOF
PB_TEST(cov_usb_usbip_v2_unsupported_version_disconnects) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  uint8_t bad[8] = {0x02, 0x00, 0x80, 0x05, 0, 0, 0, 0};  // version 0x0200
  cov_send_raw(c, std::vector<uint8_t>(bad, bad + 8));
  uint8_t b = 0;
  std::string e2;
  PB_CHECK(!c.recv_exact(&b, 1, &e2));  // 服务器 break 后关闭连接
  c.close();
  srv.stop();
}

// IMPORT 两类拒绝（全零 busid 解析失败 / 未知 busid）都回 REP_IMPORT status=1 且不占用
// 连接：拒绝后 DEVLIST 仍正常应答（server.cpp:177-182, 185-188, 201-204）
PB_TEST(cov_usb_usbip_v2_import_rejects_keep_control_loop) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;  // 空注册表
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());

  uint8_t zeros[32] = {};
  cov2_send_op(c, OP_REQ_IMPORT, zeros, sizeof(zeros));  // parse_import_busid 失败
  auto r1 = cov_recv_n(c, 8);
  PB_CHECK_EQ(r1[3], OP_REP_IMPORT & 0xFF);
  PB_CHECK_EQ(r1[7], 0x01);  // status = error
  char b9[32] = {};
  b9[0] = '9';
  b9[1] = '-';
  b9[2] = '9';
  cov2_send_op(c, OP_REQ_IMPORT, b9, sizeof(b9));  // reg.find 失败 → 警告路径
  auto r2 = cov_recv_n(c, 8);
  PB_CHECK_EQ(r2[3], OP_REP_IMPORT & 0xFF);
  PB_CHECK_EQ(r2[7], 0x01);

  // 两次拒绝均 continue：控制循环仍在服务
  cov2_send_op(c, OP_REQ_DEVLIST);
  auto head = cov_recv_n(c, 12);
  PB_CHECK_EQ(head[3], OP_REP_DEVLIST & 0xFF);
  PB_CHECK_EQ(head[11], 0);  // ndev = 0
  c.close();
  srv.stop();
}

// IMPORT 成功（REP_IMPORT status=0 + 312B 设备，server.cpp:193-199）后 EP0 控制传输：
// IN 取模型应答（actual=min(resp,wLength)），OUT 读 data 阶段（actual=data.size()，
// server.cpp:262-264, 276-280）；连接关闭 → serve_urb_loop 退出 → 清理路径
//（on_detach + entry->ctx 置空 + conn.close，server.cpp:220-236）
PB_TEST(cov_usb_usbip_v2_import_ep0_in_out_control) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovV2Model>("7-12");
  PB_CHECK_EQ(reg.add(model), std::string("7-12"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov2_import(c, "7-12");

  // EP0 IN：模型应答 {0x12,0x01}，wLength=4 → actual=2，数据紧跟 48B 头
  uint8_t setup_in[8] = {0x80, 0x06, 0, 1, 0, 0, 4, 0};
  cov_send_submit(c, 40, 0, DIR_IN, 4, setup_in);
  auto ret = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ(ret[3], 0x03);           // RET_SUBMIT
  PB_CHECK_EQ(ret[7], 40);             // seqnum 低字节
  PB_CHECK_EQ(cov_be32(ret, 20), 0);   // status = SUCCESS
  PB_CHECK_EQ(cov_be32(ret, 24), 2);   // actual = min(2, 4)
  PB_CHECK_EQ(ret[48], 0x12);
  PB_CHECK_EQ(ret[49], 0x01);

  // EP0 OUT 带 2B data 阶段：读满后交模型，actual = data.size()，OUT 无数据阶段应答
  uint8_t setup_out[8] = {0x21, 0x09, 0, 2, 0, 0, 2, 0};
  cov_send_submit(c, 41, 0, DIR_OUT, 2, setup_out, {0xEE, 0xFF});
  auto ret2 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret2[3], 0x03);
  PB_CHECK_EQ(ret2[7], 41);
  PB_CHECK_EQ(cov_be32(ret2, 20), 0);
  PB_CHECK_EQ(cov_be32(ret2, 24), 2);  // actual = data.size()
  PB_CHECK(cov2_spin_cnt(model->ctrl_done, 2));
  PB_CHECK_EQ(model->last_ctrl_data, (std::vector<uint8_t>{0xEE, 0xFF}));

  c.close();                                    // → recv 失败 → serve_urb_loop false → 清理
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));  // on_detach 已回调（清理路径入口）
  srv.stop();                                   // join 连接线程：entry->ctx=nullptr + close 已执行
}

// 非 EP0 数据传输：bulk OUT（读 data 阶段 → on_out，server.cpp:288-296）；IN 挂起
//（wire ep 1 → 模型地址 0x81，server.cpp:303-311）；UNLINK 命中/未命中挂起 URB
//（server.cpp:315-338，含 324 行 unlink_seqnum 组装）
PB_TEST(cov_usb_usbip_v2_bulk_out_pending_in_unlink) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-13");
  PB_CHECK_EQ(reg.add(model), std::string("7-13"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov2_import(c, "7-13");

  uint8_t zero8[8] = {};
  // bulk OUT（wire ep=2）：模型收到 on_out(2, {1,2,3})，RET actual=tbl
  cov_send_submit(c, 50, 2, DIR_OUT, 3, zero8, {1, 2, 3});
  auto r = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(r[3], 0x03);
  PB_CHECK_EQ(r[7], 50);
  PB_CHECK_EQ(r[19], 2);              // ep 回显（wire 端点号）
  PB_CHECK_EQ(cov_be32(r, 20), 0);
  PB_CHECK_EQ(cov_be32(r, 24), 3);
  PB_CHECK(cov2_spin_cnt(model->out_calls, 1));  // on_out 内先写后计数：可安全读字段
  PB_CHECK_EQ(model->last_out_ep, 2);
  PB_CHECK_EQ(model->last_out_data, (std::vector<uint8_t>{1, 2, 3}));

  // IN 提交：服务器登记 pending_ins[0x81]=seq51 并回调 on_in(0x81)；模型不完成 → 无应答
  cov_send_submit(c, 51, 1, DIR_IN, 64, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 1));

  // UNLINK 命中挂起 seq 51 → status=0（同一 TCP 流有序：提交必先于 unlink 处理）
  cov_send_unlink(c, 52, 0x81, 51);
  auto ru = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ(ru[3], 0x04);
  PB_CHECK_EQ(ru[7], 52);
  PB_CHECK_EQ(cov_be32(ru, 20), 0);
  // UNLINK 未命中（挂起表已空）→ status=-1
  cov_send_unlink(c, 53, 0x81, 999);
  auto ru2 = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ(ru2[7], 53);
  PB_CHECK_EQ(cov_be32(ru2, 20), -1);

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));
  srv.stop();
}

// usb/control.cpp DT_STRING 两条路径：index 命中（break 出 switch）与未命中
//（return false 交由模型/STALL）——覆盖 descriptor_request 的字符串查找分支
PB_TEST(cov_usb_usbip_v2_control_string_descriptor_lookup_paths) {
  usb::DescriptorStore s;
  s.strings[5] = usb::build_string_descriptor("x");
  std::vector<uint8_t> out;
  // 命中 index 5：完整返回 {4, 3, 'x', 0}（wLength=4 不截断）
  PB_CHECK(usb::descriptor_request(cov_setup(0x80, usb::REQ_GET_DESCRIPTOR, 0x0305, 4), s, out));
  PB_CHECK_EQ(out.size(), 4u);
  PB_CHECK_EQ(out[0], 4u);
  PB_CHECK_EQ(out[1], 3u);
  PB_CHECK_EQ(out[2], 'x');
  // 未命中 index 7：return false（找不到字符串描述符）
  PB_CHECK(!usb::descriptor_request(cov_setup(0x80, usb::REQ_GET_DESCRIPTOR, 0x0307, 4), s, out));
}

// ============ R1 追加（第 3 批）：同端点多 URB 排队 FIFO（server.cpp:47 pending_ins）============
// 既有测试同端点挂起深度恒为 1：test_usbip_server.cpp:154/165/171 每次提交前先完成上一个
//（严格串行），cov 437-438 行是两个不同端点（0x81/0x82）。此处同端点 0x81 挂起深度 3，
// 断言 complete_in 按 deque front（提交顺序）应答；UNLINK 命中中段后剩余项仍保持 FIFO。

namespace {

// FIFO 模型：on_in 仅登记不完成（保证挂起深度 >1）；on_out 原子取走 drain 队列并按序
// complete_in(0x81)。drain 由测试线程在发送触发 OUT 之前入队（互斥锁同步，跨线程安全）。
struct CovFifoModel : CovModel {
  explicit CovFifoModel(const char* busid) : CovModel(busid) {}
  void on_out(uint8_t ep, std::vector<uint8_t> data) override {
    CovModel::on_out(ep, std::move(data));
    if (!sink) return;
    std::vector<std::vector<uint8_t>> take;
    {
      std::lock_guard<std::mutex> lk(drain_mx);
      take.swap(drain);
    }
    // 在服务线程的 on_out 内同步完成：RET 应答顺序 = 完成调用顺序，先于 OUT 自身应答
    for (auto& p : take) sink->complete_in(0x81, 0, p);
  }
  void push_drain(std::vector<uint8_t> p) {
    std::lock_guard<std::mutex> lk(drain_mx);
    drain.push_back(std::move(p));
  }
  std::mutex drain_mx;
  std::vector<std::vector<uint8_t>> drain;
};

}  // namespace

PB_TEST(cov_usb_usbip_server_same_ep_multi_urb_fifo) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifoModel>("7-14");
  PB_CHECK_EQ(reg.add(model), std::string("7-14"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-14");

  uint8_t zero8[8] = {};
  // —— 第一轮：同端点 0x81 连挂 3 个 IN（深度 3），完成按提交顺序应答 ——
  cov_send_submit(c, 60, 1, DIR_IN, 8, zero8);  // pending[0x81] = [60]
  cov_send_submit(c, 61, 1, DIR_IN, 8, zero8);  // [60, 61]
  cov_send_submit(c, 62, 1, DIR_IN, 8, zero8);  // [60, 61, 62]
  PB_CHECK(cov2_spin_cnt(model->in_calls, 3));  // 三个均已入队且未完成（深度保持 3）

  model->push_drain({0xA0});
  model->push_drain({0xA1, 0xA2});
  model->push_drain({0xA3, 0xA4, 0xA5});
  cov_send_submit(c, 63, 2, DIR_OUT, 0, zero8);  // on_out 排空 drain：3 次 complete_in
  // 同一 TCP 流有序：RET(60)+1B → RET(61)+2B → RET(62)+3B → OUT 自身 RET(63)
  auto r60 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 1);
  PB_CHECK_EQ(r60[3], 0x03);              // RET_SUBMIT
  PB_CHECK_EQ(r60[7], 60);                // 最先提交的 seq 最先应答（FIFO front）
  PB_CHECK_EQ(r60[19], 0x81);             // ep 回显（含方向位的模型地址）
  PB_CHECK_EQ(cov_be32(r60, 20), 0);      // status SUCCESS
  PB_CHECK_EQ(cov_be32(r60, 24), 1);      // actual = 第 1 次完成的数据长
  PB_CHECK_EQ(r60[48], 0xA0);
  auto r61 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ(r61[7], 61);                // 第 2 项
  PB_CHECK_EQ(cov_be32(r61, 24), 2);
  PB_CHECK_EQ(r61[48], 0xA1);
  PB_CHECK_EQ(r61[49], 0xA2);
  auto r62 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 3);
  PB_CHECK_EQ(r62[7], 62);                // 第 3 项（最后提交的最后应答）
  PB_CHECK_EQ(cov_be32(r62, 24), 3);
  PB_CHECK_EQ(r62[48], 0xA3);
  PB_CHECK_EQ(r62[50], 0xA5);
  auto r63 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(r63[7], 63);                // OUT 自身应答在 3 次完成之后
  PB_CHECK_EQ(cov_be32(r63, 24), 0);

  // —— 第二轮：深度 3 时 UNLINK 中间项，剩余两项仍按 FIFO 完成 ——
  cov_send_submit(c, 70, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 71, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 72, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 6));
  cov_send_unlink(c, 80, 0x81, 71);  // 摘除中段 seq71（deque 中段 erase，server.cpp:334）
  auto ru = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ(ru[3], 0x04);
  PB_CHECK_EQ(ru[7], 80);
  PB_CHECK_EQ(cov_be32(ru, 20), 0);  // 命中挂起项

  model->push_drain({0xB0});
  model->push_drain({0xB1, 0xB2});
  cov_send_submit(c, 73, 2, DIR_OUT, 0, zero8);
  // FIFO：先应答队首 70，再应答 72（71 已被 unlink 摘除）
  auto r70 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 1);
  PB_CHECK_EQ(r70[7], 70);
  PB_CHECK_EQ(cov_be32(r70, 24), 1);
  PB_CHECK_EQ(r70[48], 0xB0);
  auto r72 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ(r72[7], 72);
  PB_CHECK_EQ(cov_be32(r72, 24), 2);
  PB_CHECK_EQ(r72[48], 0xB1);
  auto r73 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(r73[7], 73);
  PB_CHECK_EQ(model->out_calls.load(), 2);  // 两轮各触发一次 on_out

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));  // 连接关闭 → 清理路径
  srv.stop();
}

// ============ 缺陷修复批（bug_double-import）：USB/IP 单导出单导入 ============
// 缺陷：同一设备被第二个连接 IMPORT 时，原实现只检查“本连接是否已绑定”
//（server.cpp ctx.model），跨连接无占用登记 → 第二个连接成功绑定并
// attach_sink 覆盖首个连接的 sink —— 首个连接的挂起 IN 永远等不到完成（wedged）。
// 正确行为：USB/IP 单导出单导入，已被导入的设备对第二个连接回 REP_IMPORT
// status=1 错误应答，不覆盖 sink；导入连接断开后占用释放、可再次导入。
// 同步全部走回环 TCP 请求/应答 + atomic 事件标志；SO_RCVTIMEO 仅在缺陷
// 复现（sink 被覆盖 → 应答永不到达）时把无限阻塞收敛为 3s 超时失败。
#include <winsock2.h>

namespace {

// 记录 attach_sink 调用与当前 sink 指针（atomic：跨线程观测，缺陷复现时
// 第二个连接的 attach_sink 会覆盖首个连接的 sink）
struct CovImportModel : CovModel {
  explicit CovImportModel(const char* busid) : CovModel(busid) {}
  void attach_sink(usb::IUsbModelSink* s) override {
    CovModel::attach_sink(s);
    sink_now.store(s);
    ++attach_calls;
  }
  std::atomic<usb::IUsbModelSink*> sink_now{nullptr};
  std::atomic<int> attach_calls{0};
};

// 有界导入尝试：返回 status 字节（0x00=成功且已消费 312B 设备，0x01=拒绝）
uint8_t bug_import_status(net::TcpConn& c, const char* busid) {
  char b[32] = {};
  for (size_t i = 0; i < 32 && busid[i]; ++i) b[i] = busid[i];
  cov2_send_op(c, OP_REQ_IMPORT, b, sizeof(b));
  auto imp = cov_recv_n(c, 8);
  if (imp[3] != (OP_REP_IMPORT & 0xFF))
    throw std::runtime_error("bug_import_status: reply is not REP_IMPORT");
  if (imp[7] == 0x00) cov_recv_n(c, USB_DEVICE_PACKED_SIZE);  // 成功：消费 312B 设备
  return imp[7];
}

void bug_set_recv_timeout(net::TcpConn& c, DWORD ms) {
  setsockopt((SOCKET)c.raw(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

}  // namespace

// 第二个连接 IMPORT 同一设备 → 错误应答（status=1），首个连接的 sink 不被
// 覆盖（attach_calls 仍 1 / sink 指针不变），且首个连接仍能收到完成应答。
PB_TEST(bug_double_import_second_connection_rejected) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovImportModel>("7-15");
  PB_CHECK_EQ(reg.add(model), std::string("7-15"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto a = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  auto b = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(a.is_open());
  PB_CHECK(b.is_open());
  bug_set_recv_timeout(a, 3000);  // 缺陷复现时收敛为超时失败而非挂死
  bug_set_recv_timeout(b, 3000);
  cov2_import(a, "7-15");
  PB_CHECK(cov2_spin_cnt(model->attach_calls, 1));
  usb::IUsbModelSink* sink_a = model->sink_now.load();

  // 第二个连接 IMPORT 同一 busid → REP_IMPORT status=1（单导出单导入）
  PB_CHECK_EQ(bug_import_status(b, "7-15"), 0x01);
  // sink 未被覆盖：无第二次 attach_sink，指针仍是首个连接的
  PB_CHECK_EQ(model->attach_calls.load(), 1);
  PB_CHECK(model->sink_now.load() == sink_a);
  // 被拒连接仍留在控制循环：DEVLIST 正常应答
  cov2_send_op(b, OP_REQ_DEVLIST);
  auto head = cov_recv_n(b, 12);
  PB_CHECK_EQ(head[3], OP_REP_DEVLIST & 0xFF);
  PB_CHECK_EQ(head[11], 1);  // ndev = 1

  // 首个连接的挂起 IN 仍由其 sink 完成：提交 seq90 → 开 STALL → RET(EPIPE, seq90)
  uint8_t zero8[8] = {};
  cov_send_submit(a, 90, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 1));  // seq90 已登记 pending[0x81]
  model->stall_on_in = true;
  cov_send_submit(a, 91, 1, DIR_IN, 8, zero8);  // on_in → stall_ep 应答队首 seq90
  auto r90 = cov_recv_n(a, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(r90[3], 0x03);
  PB_CHECK_EQ(r90[7], 90);
  PB_CHECK_EQ(cov_be32(r90, 20), URB_STATUS_EPIPE);

  a.close();
  b.close();
  srv.stop();
}

// 首个导入连接断开 → 占用释放 → 另一连接可成功 IMPORT（不得永久锁死设备）
PB_TEST(bug_double_import_release_after_detach_allows_reimport) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovImportModel>("7-16");
  PB_CHECK_EQ(reg.add(model), std::string("7-16"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto a = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  auto b = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(a.is_open());
  PB_CHECK(b.is_open());
  bug_set_recv_timeout(a, 3000);
  bug_set_recv_timeout(b, 3000);
  cov2_import(a, "7-16");
  PB_CHECK(cov2_spin_cnt(model->attach_calls, 1));

  // 占用中：拒绝
  PB_CHECK_EQ(bug_import_status(b, "7-16"), 0x01);

  // 首个连接断开 → 清理路径（on_detach 前）释放占用
  a.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));

  // 释放后：第二个连接成功导入（status=0）
  PB_CHECK_EQ(bug_import_status(b, "7-16"), 0x00);
  PB_CHECK(cov2_spin_cnt(model->attach_calls, 2));  // 应答先于 attach_sink 发出，计数用有界等待
  b.close();
  srv.stop();
}

// ============ bug_resize-clamp：CMD_SUBMIT transfer_buffer_length 钳位 ============
// 缺陷：tbl 由对端完全控制（int32），原实现直接 data.resize(tbl) → 超限值造成
// 巨额分配/bad_alloc 杀死连接线程甚至进程。正确行为：tbl > MAX_TRANSFER_BUFFER
// 时不读数据阶段、不触碰模型，立即回 RET_SUBMIT(status=EINVAL)，连接继续服务。

PB_TEST(bug_resize_clamp_rejects_oversized_transfer_buffer) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-30");
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-30");

  // 非 EP0 OUT：tbl = 上限+1（不附带数据阶段），须回 EINVAL(-22)、actual=0
  uint8_t zero8[8] = {};
  cov_send_submit(c, 40, 2, DIR_OUT, MAX_TRANSFER_BUFFER + 1, zero8);
  auto ret = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret[3], 0x03);  // RET_SUBMIT
  PB_CHECK_EQ(ret[7], 40);    // seq 回显
  PB_CHECK_EQ(cov_be32(ret, 20), URB_STATUS_EINVAL);
  PB_CHECK_EQ(cov_be32(ret, 24), 0);
  PB_CHECK_EQ(model->out_calls.load(), 0);  // 模型未被触碰（也未试图读 17MB 数据阶段）

  // EP0 OUT：同一钳位须先于控制传输的数据阶段读取生效
  uint8_t setup[8] = {0x21, 0x09, 0, 0, 0, 0, 0, 0};  // HID SET_REPORT
  cov_send_submit(c, 41, 0, DIR_OUT, MAX_TRANSFER_BUFFER + 1, setup);
  auto ret2 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret2[7], 41);
  PB_CHECK_EQ(cov_be32(ret2, 20), URB_STATUS_EINVAL);
  PB_CHECK_EQ(cov_be32(ret2, 24), 0);
  PB_CHECK_EQ(model->control_calls.load(), 0);

  // 钳位后连接仍在服务：正常小包 OUT 照旧成功
  cov_send_submit(c, 42, 2, DIR_OUT, 3, zero8, {1, 2, 3});
  auto ret3 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret3[7], 42);
  PB_CHECK_EQ(cov_be32(ret3, 20), URB_STATUS_SUCCESS);
  PB_CHECK_EQ(cov_be32(ret3, 24), 3);
  PB_CHECK_EQ(model->out_calls.load(), 1);
  c.close();
  srv.stop();
}

PB_TEST(bug_resize_clamp_boundary_16mb_still_served) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovModel>("7-31");
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-31");

  // 恰好等于上限：合法传输，须走原路径成功应答（钳位只拒绝“超过”上限的值）
  uint8_t zero8[8] = {};
  std::vector<uint8_t> big((size_t)MAX_TRANSFER_BUFFER, 0x5A);
  cov_send_submit(c, 50, 2, DIR_OUT, MAX_TRANSFER_BUFFER, zero8, big);
  auto ret = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(ret[7], 50);
  PB_CHECK_EQ(cov_be32(ret, 20), URB_STATUS_SUCCESS);
  PB_CHECK_EQ(cov_be32(ret, 24), MAX_TRANSFER_BUFFER);  // actual = tbl
  PB_CHECK_EQ(model->out_calls.load(), 1);
  PB_CHECK_EQ(model->last_out_data.size(), (size_t)MAX_TRANSFER_BUFFER);
  PB_CHECK_EQ(model->last_out_data.front(), 0x5A);
  PB_CHECK_EQ(model->last_out_data.back(), 0x5A);
  c.close();
  srv.stop();
}

// ============ bug_stop-uaf：stop() 先 join 连接线程再 delete ConnEntry ============
// 缺陷（修复前）：stop() 在 join 之前 delete ConnEntry，而连接线程退出路径
// （handle_connection 尾部）仍会写 entry->ctx = nullptr → 对已释放内存的写入。
// 锚定：连接线程阻塞在 on_detach（恰在 ctx 写入之前），stop() 同步推进；放行
// 线程前喷洒与 ConnEntry 同尺寸的 canary 块占住释放后的内存——若 stop() 先
// delete（缺陷序），线程随后的 ctx=nullptr 写入会把 canary 内某处清出 8 字节零串
// （ctx 实测偏移 16）→ 全缓冲扫描可检出。修复序（先 join 后 delete）下 canary 全程由本测试独占
// 持有、绝无写入 → 确定性通过。多轮次提高对缺陷序的检出概率。

namespace {
struct BugStopUafModel : usb::IUsbDeviceModel {
  explicit BugStopUafModel(const char* busid) {
    dev_.path = std::string("/phantom/") + busid;
    dev_.busid = busid;
  }
  const usbip::ExportedDevice& exported() const override { return dev_; }
  void attach_sink(usb::IUsbModelSink*) override {}
  std::vector<uint8_t> on_control(const usb::ControlSetup&, const std::vector<uint8_t>&) override {
    return {};
  }
  void on_out(uint8_t, std::vector<uint8_t>) override {}
  void on_in(uint8_t) override {}
  // 连接线程清理路径先于 entry->ctx 写入到达此处：原地自旋直到测试放行
  void on_detach() override {
    entered.store(true);
    while (!release.load()) std::this_thread::yield();
    ++exited;
  }
  usbip::ExportedDevice dev_;
  std::atomic<bool> entered{false}, release{false};
  std::atomic<int> exited{0};
};

// 单轮：返回是否检出异常（canary 被悬垂写入击中，或连接线程未在 join 前走完）
bool bug_stop_uaf_round(net::WsaSession& wsa, const char* busid) {
  sim::DeviceRegistry reg;
  auto model = std::make_shared<BugStopUafModel>(busid);
  reg.add(model);
  Server srv(reg, 0);
  std::string err;
  if (!srv.start(&err)) return false;
  {
    auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
    if (!c.is_open()) return false;
    cov_import(c, busid);
    c.close();  // 服务端 recv 失败 → 清理路径 → on_detach 阻塞（ctx 写入之前）
  }
  if (!cov_spin_until(model->entered)) return false;

  std::thread stopper([&] { srv.stop(); });
  // stop() 必经路径：关 listener → join accept 线程 → 关各连接 socket →
  // （缺陷序：此刻已 delete ConnEntry）→ join 连接线程阻塞。为同步推进留出
  // 余量（本套件在确需真实时间窗口处允许短暂 Sleep，见 test_usbip_server.cpp）。
  Sleep(50);

  // 喷 canary：ConnEntry = {std::thread(16B) + ConnCtx*} = 24B（实测），覆盖相邻
  // 尺寸类；保持分配直到扫描完成（缺陷序下 freed ConnEntry 会落入其中之一）
  const size_t kSizes[] = {16, 24, 32, 40, 48};
  std::vector<std::pair<std::unique_ptr<char[]>, size_t>> canaries;
  for (size_t sz : kSizes) {
    for (int i = 0; i < 256; ++i) {
      char* p = new char[sz];
      std::memset(p, 0xEE, sz);
      canaries.emplace_back(std::unique_ptr<char[]>(p), sz);
    }
  }

  model->release.store(true);  // 放行：缺陷序下线程随即写 entry->ctx = nullptr
  stopper.join();
  bool bad = model->exited.load() != 1;  // 连接线程未在 join 前走完即异常
  for (auto& cn : canaries) {
    const char* p = cn.first.get();
    size_t sz = cn.second;
    for (size_t k = 0; k + 8 <= sz; ++k) {
      bool zeroed = true;
      for (size_t m = k; m < k + 8; ++m)
        if (p[m] != 0) zeroed = false;
      if (zeroed) bad = true;
    }
  }
  return bad;
}
}  // namespace

PB_TEST(bug_stop_uaf_stop_joins_before_connentry_delete) {
  net::WsaSession wsa;
  // 多轮次：缺陷序的 canary 复用依赖分配器归还行为，单轮可能漏检；
  // 修复序确定性通过（canary 全程被本测试独占，不存在悬垂写入）
  bool detected = false;
  for (int round = 0; round < 5 && !detected; ++round)
    detected = bug_stop_uaf_round(wsa, "7-32");
  PB_CHECK(!detected);
}

// ============ R2 追加：多 URB 同端点排队 FIFO 的补强断言 ============
// 矩阵行「多 URB 同端点排队 FIFO」（server.cpp:52 pending_ins 每端点 deque）。
// R1 第 3 批（上方 cov_usb_usbip_server_same_ep_multi_urb_fifo）已断言深度 3 的
// 一次性排空顺序与 UNLINK 中段摘除。R2 补三类仍未断言的行为：
//   1) 分步完成：深度 3 逐次 complete_in（每轮触发只完成队首 1 个），两次完成
//      之间其余项保持挂起，应答序恒为提交序；
//   2) 双端点各深度 2 且完成顺序跨端点交错：每端点独立 FIFO，不串队；
//   3) stall_ep 深度 >1 时只消费队首（server.cpp:411-414）：队首回 EPIPE，
//      其余项仍在队列并按 FIFO 顺序完成（不丢弃、不越位）。
// 同步仍走回环 TCP 请求/应答 + atomic 事件标志；结尾「信道安静」检查用
// SO_RCVTIMEO(300ms)+MSG_PEEK 做有界负向断言（队列排空后不得有残留应答），
// 与上方 bug 系列的超时收敛（bug_set_recv_timeout）同型。

namespace {

// 多端点 FIFO 模型：drain 每项带目标端点地址；on_out 原子取走整批并按入队序
// complete_in（在服务线程的 on_out 内同步完成 → RET 应答顺序 = 完成调用顺序，
// 且全部先于 OUT 自身的 RET 应答）。
struct CovFifo2Model : CovModel {
  explicit CovFifo2Model(const char* busid) : CovModel(busid) {}
  void on_out(uint8_t ep, std::vector<uint8_t> data) override {
    CovModel::on_out(ep, std::move(data));
    if (!sink) return;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> take;
    {
      std::lock_guard<std::mutex> lk(drain_mx);
      take.swap(drain);
    }
    for (auto& kv : take) sink->complete_in(kv.first, 0, kv.second);
  }
  void push_drain(uint8_t ep_addr, std::vector<uint8_t> p) {
    std::lock_guard<std::mutex> lk(drain_mx);
    drain.emplace_back(ep_addr, std::move(p));
  }
  std::mutex drain_mx;
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> drain;
};

// 信道安静：300ms 内窥探不得有任何残留字节（超时=通过；窥见数据=失败）。
// 只在测试收尾（所有期望应答已被精确读尽）后调用。
bool cov_r2_channel_quiet(net::TcpConn& c) {
  DWORD ms = 300;
  setsockopt((SOCKET)c.raw(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
  char b = 0;
  int r = recv((SOCKET)c.raw(), &b, 1, MSG_PEEK);
  return r == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT;
}

}  // namespace

// 1) 同端点 0x81 深度 3 的分步完成：每轮 OUT 触发只排空 1 项，应答恒为当前
// 队首（提交序）；中间状态下队列持有剩余项、不产生任何应答。
PB_TEST(cov_usb_usbip_r2_same_ep_fifo_stepwise_completion) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifo2Model>("7-40");
  PB_CHECK_EQ(reg.add(model), std::string("7-40"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-40");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 100, 1, DIR_IN, 8, zero8);  // pending[0x81] = [100, 101, 102]
  cov_send_submit(c, 101, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 102, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 3));  // 三项均已挂起且无一完成（深度保持 3）

  const int drain_seqs[3] = {100, 101, 102};  // 每轮应答的队首 = 提交序
  const int trig_seqs[3] = {103, 104, 105};
  const std::vector<std::vector<uint8_t>> payloads = {
      {0xE0}, {0xE1, 0xE2}, {0xE3, 0xE4, 0xE5}};
  for (int k = 0; k < 3; ++k) {
    model->push_drain(0x81, payloads[k]);  // 本轮只放行 1 项
    cov_send_submit(c, (uint32_t)trig_seqs[k], 2, DIR_OUT, 0, zero8);
    // 同一 TCP 流有序：先 RET(队首完成) 后 RET(OUT 自身)；任何乱序/超前/重复
    // 应答都会使下面的 seq/actual 断言失配
    auto rk = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + payloads[k].size());
    PB_CHECK_EQ(rk[3], 0x03);
    PB_CHECK_EQ((int)rk[7], drain_seqs[k]);  // 第 k 轮应答第 k 个提交（FIFO front）
    PB_CHECK_EQ((int)rk[19], 0x81);          // ep 回显（含方向位的模型地址）
    PB_CHECK_EQ(cov_be32(rk, 20), 0);        // status SUCCESS
    PB_CHECK_EQ(cov_be32(rk, 24), (int32_t)payloads[k].size());
    for (size_t j = 0; j < payloads[k].size(); ++j)
      PB_CHECK_EQ((int)rk[48 + j], (int)payloads[k][j]);
    auto rt = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
    PB_CHECK_EQ((int)rt[7], trig_seqs[k]);  // OUT 自身应答在完成应答之后
    PB_CHECK_EQ(cov_be32(rt, 24), 0);
  }
  PB_CHECK_EQ(model->in_calls.load(), 3);   // 无额外 on_in
  PB_CHECK_EQ(model->out_calls.load(), 3);  // 三轮各一次 OUT 触发
  PB_CHECK(cov_r2_channel_quiet(c));        // 队列已空：不得有残留应答

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));  // 连接关闭 → 清理路径
  srv.stop();
}

// 2) 双端点各深度 2（pending_ins[0x81]=[110,111]、[0x82]=[112,113]），完成顺序
// 跨端点交错（0x82→0x81→0x81→0x82）：每端点各自 FIFO，队首由本端点队列决定。
PB_TEST(cov_usb_usbip_r2_per_endpoint_fifo_independence) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifo2Model>("7-41");
  PB_CHECK_EQ(reg.add(model), std::string("7-41"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-41");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 110, 1, DIR_IN, 8, zero8);  // pending[0x81] = [110, 111]
  cov_send_submit(c, 111, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 112, 2, DIR_IN, 8, zero8);  // pending[0x82] = [112, 113]
  cov_send_submit(c, 113, 2, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 4));  // 双端点各深度 2，全部挂起

  struct Step {
    int expect_seq;  // 本步应答的队首 seq
    uint8_t expect_ep;
    std::vector<uint8_t> payload;
    int trig;  // OUT 触发的 seq
  };
  const Step steps[] = {
      {112, 0x82, {0xC1}, 120},           // 先完成 0x82 的队首
      {110, 0x81, {0xC0}, 121},           // 再完成 0x81 的队首
      {111, 0x81, {0xC2, 0xC3}, 122},     // 0x81 次项（不受 0x82 完成影响）
      {113, 0x82, {0xC4, 0xC5, 0xC6}, 123},  // 0x82 次项
  };
  for (const auto& s : steps) {
    model->push_drain(s.expect_ep, s.payload);
    cov_send_submit(c, (uint32_t)s.trig, 3, DIR_OUT, 0, zero8);
    auto rk = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + s.payload.size());
    PB_CHECK_EQ(rk[3], 0x03);
    PB_CHECK_EQ((int)rk[7], s.expect_seq);  // 应答的是该端点自己的队首
    PB_CHECK_EQ((int)rk[19], (int)s.expect_ep);
    PB_CHECK_EQ(cov_be32(rk, 20), 0);
    PB_CHECK_EQ(cov_be32(rk, 24), (int32_t)s.payload.size());
    for (size_t j = 0; j < s.payload.size(); ++j)
      PB_CHECK_EQ((int)rk[48 + j], (int)s.payload[j]);
    auto rt = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
    PB_CHECK_EQ((int)rt[7], s.trig);
  }
  PB_CHECK_EQ(model->in_calls.load(), 4);
  PB_CHECK_EQ(model->out_calls.load(), 4);
  PB_CHECK(cov_r2_channel_quiet(c));  // 两队列均空：无残留

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));
  srv.stop();
}

// 3) stall_ep 在同端点深度 >1 时只消费队首（server.cpp:411-414）：队首 130 回
// EPIPE；次项 131 不被应答、不被丢弃，随后仍按 FIFO 完成。
PB_TEST(cov_usb_usbip_r2_same_ep_stall_pops_head_only) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifo2Model>("7-42");
  PB_CHECK_EQ(reg.add(model), std::string("7-42"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-42");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 130, 1, DIR_IN, 8, zero8);  // pending[0x81] = [130]
  PB_CHECK(cov2_spin_cnt(model->in_calls, 1));

  model->stall_on_in.store(true);
  cov_send_submit(c, 131, 1, DIR_IN, 8, zero8);  // 入队 [130,131] → on_in → stall_ep
  auto r130 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(r130[3], 0x03);
  PB_CHECK_EQ((int)r130[7], 130);  // 队首 130 被 STALL 应答（不是后到的 131）
  PB_CHECK_EQ((int)r130[19], 0x81);
  PB_CHECK_EQ(cov_be32(r130, 20), URB_STATUS_EPIPE);
  PB_CHECK_EQ(cov_be32(r130, 24), 0);
  PB_CHECK_EQ(model->in_calls.load(), 2);  // 131 已入队但未被应答（深度仍 1）

  model->stall_on_in.store(false);
  model->push_drain(0x81, {0xD1, 0xD2});
  cov_send_submit(c, 132, 2, DIR_OUT, 0, zero8);
  auto r131 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);  // 剩余项 131 仍按 FIFO 完成
  PB_CHECK_EQ((int)r131[7], 131);
  PB_CHECK_EQ((int)r131[19], 0x81);
  PB_CHECK_EQ(cov_be32(r131, 20), 0);
  PB_CHECK_EQ(cov_be32(r131, 24), 2);
  PB_CHECK_EQ((int)r131[48], 0xD1);
  PB_CHECK_EQ((int)r131[49], 0xD2);
  auto r132 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r132[7], 132);
  PB_CHECK(cov_r2_channel_quiet(c));  // 131 恰好被应答一次：无残留

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));
  srv.stop();
}

// ============ R3 追加：同端点多 URB FIFO 的剩余未断言路径 ============
// 矩阵行「多 URB 同端点排队 FIFO」（server.cpp:52 pending_ins 每端点 deque）。
// R1 第 3 批（cov_usb_usbip_server_same_ep_multi_urb_fifo）已断言深度 3 一次性排空
// 顺序与 UNLINK 中段摘除；R2 已断言分步完成、双端点独立与 stall_ep 只消费队首。
// R3 补最后四类仍未断言的行为：
//   1) UNLINK 摘队首（deque begin erase，server.cpp:361-363）后剩余项仍按 FIFO 完成；
//   2) UNLINK 摘队尾后剩余项仍按 FIFO 完成；
//   3) 主机补枪（re-arm）：完成队首后立即补提新 IN（深度回 2），既有次项不被插队；
//   4) UNLINK 清空队列（map 表项抹除，server.cpp:368）后迟到 complete_in 走早退
//      （server.cpp:394-397），不产生 RET、不错配已 unlink 的 seq；同端点新 IN 照常完成。
// 同步仍走回环 TCP 请求/应答 + atomic 事件标志，无时序假设；「信道安静」负向断言
// 仅在收尾（所有期望应答已精确读尽）后使用（cov_r2_channel_quiet，SO_RCVTIMEO+PEEK）。

// 1)+2) 深度 3 下 UNLINK 分别摘队首 / 摘队尾：剩余两项均按提交顺序完成，且被
// unlink 的 seq 全程无补发应答（deque 两端 erase 均不破坏中段次序）。
PB_TEST(cov_usb_usbip_r3_same_ep_unlink_head_or_tail_keep_fifo) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifo2Model>("7-50");
  PB_CHECK_EQ(reg.add(model), std::string("7-50"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-50");

  uint8_t zero8[8] = {};
  // —— 摘队首：pending[0x81] = [200,201,202]，unlink 200 → 剩 [201,202] ——
  cov_send_submit(c, 200, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 201, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 202, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 3));  // 三项均已挂起且无一完成（深度保持 3）
  cov_send_unlink(c, 203, 0x81, 200);           // 摘队首（deque begin erase）
  auto ru = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ(ru[3], 0x04);
  PB_CHECK_EQ((int)ru[7], 203);
  PB_CHECK_EQ(cov_be32(ru, 20), 0);  // 命中挂起项

  model->push_drain(0x81, {0xA0});
  model->push_drain(0x81, {0xA1, 0xA2});
  cov_send_submit(c, 204, 2, DIR_OUT, 0, zero8);       // on_out 排空 2 项
  auto r201 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 1);  // 完成应答先于 OUT 自身应答
  PB_CHECK_EQ(r201[3], 0x03);
  PB_CHECK_EQ((int)r201[7], 201);  // 摘队首后由原次项顶上（不是 202）
  PB_CHECK_EQ((int)r201[19], 0x81);
  PB_CHECK_EQ(cov_be32(r201, 20), 0);
  PB_CHECK_EQ(cov_be32(r201, 24), 1);
  PB_CHECK_EQ((int)r201[48], 0xA0);
  auto r202 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ((int)r202[7], 202);  // 第 3 项仍最后应答
  PB_CHECK_EQ(cov_be32(r202, 24), 2);
  PB_CHECK_EQ((int)r202[48], 0xA1);
  PB_CHECK_EQ((int)r202[49], 0xA2);
  auto r204 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r204[7], 204);

  // —— 摘队尾：pending[0x81] = [210,211,212]，unlink 212 → 剩 [210,211] ——
  cov_send_submit(c, 210, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 211, 1, DIR_IN, 8, zero8);
  cov_send_submit(c, 212, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 6));
  cov_send_unlink(c, 213, 0x81, 212);  // 摘队尾
  auto ru2 = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ((int)ru2[7], 213);
  PB_CHECK_EQ(cov_be32(ru2, 20), 0);

  model->push_drain(0x81, {0xB0});
  model->push_drain(0x81, {0xB1, 0xB2});
  cov_send_submit(c, 214, 2, DIR_OUT, 0, zero8);
  auto r210 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 1);
  PB_CHECK_EQ((int)r210[7], 210);  // 队首不受摘尾影响
  PB_CHECK_EQ((int)r210[48], 0xB0);
  auto r211 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ((int)r211[7], 211);
  PB_CHECK_EQ((int)r211[48], 0xB1);
  PB_CHECK_EQ((int)r211[49], 0xB2);
  auto r214 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r214[7], 214);
  PB_CHECK_EQ(model->out_calls.load(), 2);  // 两轮各触发一次 on_out
  PB_CHECK(cov_r2_channel_quiet(c));        // 被摘除的 200/212 无任何补发应答

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));  // 连接关闭 → 清理路径
  srv.stop();
}

// 3) 主机补枪（re-arm）穿插：完成队首后立即补提一个新 IN（深度回到 2），
// 既有次项保持队首地位、不被新提交插队 —— 完整穿插序列下 FIFO 全序保持。
PB_TEST(cov_usb_usbip_r3_same_ep_interleaved_rearm_keeps_fifo) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifo2Model>("7-51");
  PB_CHECK_EQ(reg.add(model), std::string("7-51"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-51");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 220, 1, DIR_IN, 8, zero8);  // pending[0x81] = [220, 221]
  cov_send_submit(c, 221, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 2));

  // 完成 220 → 队列 [221]
  model->push_drain(0x81, {0xC0});
  cov_send_submit(c, 222, 2, DIR_OUT, 0, zero8);
  auto r220 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 1);
  PB_CHECK_EQ(r220[3], 0x03);
  PB_CHECK_EQ((int)r220[7], 220);
  PB_CHECK_EQ((int)r220[19], 0x81);
  PB_CHECK_EQ(cov_be32(r220, 20), 0);
  PB_CHECK_EQ((int)r220[48], 0xC0);
  auto r222 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r222[7], 222);

  // 补枪：新 IN 追加队尾 → [221, 223]（221 不得被插队）
  cov_send_submit(c, 223, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 3));

  model->push_drain(0x81, {0xC1, 0xC2});
  cov_send_submit(c, 224, 2, DIR_OUT, 0, zero8);
  auto r221 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ((int)r221[7], 221);  // 先完成穿插前挂起的 221（非后到的 223）
  PB_CHECK_EQ((int)r221[19], 0x81);
  PB_CHECK_EQ(cov_be32(r221, 20), 0);
  PB_CHECK_EQ((int)r221[48], 0xC1);
  PB_CHECK_EQ((int)r221[49], 0xC2);
  auto r224 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r224[7], 224);

  // 队列 [223]：最后一次完成归属补提项
  model->push_drain(0x81, {0xC3, 0xC4, 0xC5});
  cov_send_submit(c, 225, 2, DIR_OUT, 0, zero8);
  auto r223 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 3);
  PB_CHECK_EQ((int)r223[7], 223);
  PB_CHECK_EQ((int)r223[48], 0xC3);
  PB_CHECK_EQ((int)r223[50], 0xC5);
  auto r225 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r225[7], 225);
  PB_CHECK_EQ(model->in_calls.load(), 3);   // 恰 3 次提交，无多余 on_in
  PB_CHECK_EQ(model->out_calls.load(), 3);  // 三轮各一次 OUT 触发
  PB_CHECK(cov_r2_channel_quiet(c));        // 全序消费完毕：无残留应答

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));
  srv.stop();
}

// 4) UNLINK 清空同端点队列（server.cpp:368 抹除 map 表项）后：迟到的 complete_in
// 走早退（server.cpp:394-397）——不产生任何 RET、不错配到已 unlink 的 seq；随后
// 同端点新 IN 正常登记并按 FIFO 完成。
PB_TEST(cov_usb_usbip_r3_same_ep_unlink_all_late_complete_dropped) {
  net::WsaSession wsa;
  sim::DeviceRegistry reg;
  auto model = std::make_shared<CovFifo2Model>("7-52");
  PB_CHECK_EQ(reg.add(model), std::string("7-52"));
  Server srv(reg, 0);
  std::string err;
  PB_CHECK(srv.start(&err));
  auto c = net::TcpConn::connect("127.0.0.1", srv.port(), &err);
  PB_CHECK(c.is_open());
  cov_import(c, "7-52");

  uint8_t zero8[8] = {};
  cov_send_submit(c, 230, 1, DIR_IN, 8, zero8);  // pending[0x81] = [230, 231]
  cov_send_submit(c, 231, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 2));

  cov_send_unlink(c, 232, 0x81, 230);  // 摘 230 → [231]
  auto ru1 = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ((int)ru1[7], 232);
  PB_CHECK_EQ(cov_be32(ru1, 20), 0);
  cov_send_unlink(c, 233, 0x81, 231);  // 摘 231 → 空（表项抹除）
  auto ru2 = cov_recv_n(c, RET_UNLINK_HDR_SIZE);
  PB_CHECK_EQ((int)ru2[7], 233);
  PB_CHECK_EQ(cov_be32(ru2, 20), 0);

  // 迟到完成：队列已空 → complete_in 早退。完成应答总在 OUT 自身应答之前发出
  //（server.cpp:325 先 on_out 后 reply_submit），故下一帧必为 RET(234)；若缺陷
  //（错配到已 unlink 的 seq），此处读到的是 seq≠234 的多余 RET，立即失配。
  model->push_drain(0x81, {0xF0});
  cov_send_submit(c, 234, 2, DIR_OUT, 0, zero8);
  auto r234 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ(r234[3], 0x03);
  PB_CHECK_EQ((int)r234[7], 234);  // 仅有 OUT 自身应答（迟到完成被丢弃）
  PB_CHECK_EQ(cov_be32(r234, 20), 0);
  PB_CHECK_EQ(cov_be32(r234, 24), 0);

  // 同端点新 IN：队列清空后正常复用 —— 新 seq 拿到新数据，不承接旧 payload
  cov_send_submit(c, 235, 1, DIR_IN, 8, zero8);
  PB_CHECK(cov2_spin_cnt(model->in_calls, 3));
  model->push_drain(0x81, {0xF1, 0xF2});
  cov_send_submit(c, 236, 2, DIR_OUT, 0, zero8);
  auto r235 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE + 2);
  PB_CHECK_EQ((int)r235[7], 235);
  PB_CHECK_EQ((int)r235[19], 0x81);
  PB_CHECK_EQ(cov_be32(r235, 20), 0);
  PB_CHECK_EQ((int)r235[48], 0xF1);
  PB_CHECK_EQ((int)r235[49], 0xF2);
  auto r236 = cov_recv_n(c, RET_SUBMIT_HDR_SIZE);
  PB_CHECK_EQ((int)r236[7], 236);
  PB_CHECK_EQ(model->out_calls.load(), 2);
  PB_CHECK(cov_r2_channel_quiet(c));  // 已 unlink 的 230/231 全程无补发应答

  c.close();
  PB_CHECK(cov2_spin_cnt(model->detach_calls, 1));
  srv.stop();
}
