#include "usbip/server.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/bytes.h"
#include "core/log.h"
#include "core/net.h"
#include "usb/control.h"

namespace pb::usbip {

struct Server::Impl {
  pb::sim::DeviceRegistry& reg;
  uint16_t want_port;
  std::atomic<bool> running{false};
  std::unique_ptr<net::TcpListener> listener;
  std::thread accept_thread;
  std::mutex conns_mx;
  std::vector<net::TcpConn*> conns;  // 活动连接（stop 时关闭以解除阻塞）
  uint16_t bound_port = 0;

  // 每连接上下文：绑定导入的设备 + 挂起 IN 表 + 写互斥
  struct PendingIn {
    uint32_t seqnum;
    uint32_t devid;
  };

  class ConnSink;  // 前置声明（Impl 作用域内）

  struct ConnCtx {
    net::TcpConn conn;
    std::shared_ptr<pb::usb::IUsbDeviceModel> model;
    ConnSink* sink = nullptr;
    std::mutex mx;                             // 保护 socket 写 + pending_ins
    std::map<uint8_t, PendingIn> pending_ins;  // ep → 挂起的 IN URB
    std::atomic<bool> detached{false};

    bool send_locked(const std::vector<uint8_t>& pkt) {
      std::string err;
      if (!conn.send_all(pkt.data(), pkt.size(), &err)) {
        PB_LOG_DEBUG(std::string("usbip: send failed: ") + err);
        return false;
      }
      return true;
    }
  };

  class ConnSink final : public pb::usb::IUsbModelSink {
   public:
    ConnSink(ConnCtx* c) : c_(c) {}
    void complete_in(uint8_t ep, int32_t status, std::vector<uint8_t> data) override;
    void stall_ep(uint8_t ep) override;

   private:
    ConnCtx* c_;
  };

  void accept_loop();
  void handle_connection(net::TcpConn conn);
  bool serve_urb_loop(ConnCtx& ctx);
  void reply_submit(ConnCtx& ctx, const SubmitHeader& h, int32_t status, int32_t actual,
                    const uint8_t* data, size_t data_len);
};

Server::Server(pb::sim::DeviceRegistry& reg, uint16_t port) : impl_(new Impl{reg, port}) {}
Server::~Server() { stop(); }

bool Server::start(std::string* err) {
  impl_->listener = std::make_unique<net::TcpListener>();
  if (!impl_->listener->listen(impl_->want_port, err)) return false;
  impl_->bound_port = impl_->listener->port();
  impl_->running = true;
  impl_->accept_thread = std::thread([this] { impl_->accept_loop(); });
  PB_LOG_INFO("usbip: listening on port " + std::to_string(impl_->bound_port));
  return true;
}

void Server::stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->listener) impl_->listener->close();
  {
    std::lock_guard<std::mutex> lk(impl_->conns_mx);
    for (auto* c : impl_->conns) c->close();
  }
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
}

uint16_t Server::port() const { return impl_->bound_port; }

void Server::Impl::accept_loop() {
  while (running.load()) {
    std::string err;
    auto conn = listener->accept(&err);
    if (!conn.is_open()) {
      if (running.load()) PB_LOG_WARN("usbip: accept failed: " + err);
      break;
    }
    std::thread([this, c = std::move(conn)]() mutable { handle_connection(std::move(c)); }).detach();
  }
}

void Server::Impl::handle_connection(net::TcpConn conn) {
  ConnCtx ctx;
  ctx.conn = std::move(conn);
  {
    std::lock_guard<std::mutex> lk(conns_mx);
    conns.push_back(&ctx.conn);
  }

  std::string err;
  while (running.load()) {
    uint8_t opb[4];
    if (!ctx.conn.recv_exact(opb, 4, &err)) break;
    uint32_t op = (uint32_t(opb[0]) << 24) | (uint32_t(opb[1]) << 16) | (uint32_t(opb[2]) << 8) | opb[3];

    if (op == OP_REQ_DEVLIST) {
      auto pkt = pack_rep_devlist(reg.list());
      std::lock_guard<std::mutex> lk(ctx.mx);
      if (!ctx.send_locked(pkt)) break;
      continue;
    }
    if (op == OP_REQ_IMPORT) {
      uint8_t busidb[32];
      if (!ctx.conn.recv_exact(busidb, 32, &err)) break;
      std::string busid;
      if (!parse_import_busid(busidb, sizeof(busidb), busid)) {
        auto pkt = pack_op_header(OP_REP_IMPORT, 1);
        std::lock_guard<std::mutex> lk(ctx.mx);
        ctx.send_locked(pkt);
        continue;
      }
      auto model = reg.find(busid);
      std::vector<uint8_t> pkt;
      if (!model) {
        PB_LOG_WARN("usbip: import unknown busid " + busid);
        pkt = pack_op_header(OP_REP_IMPORT, 1);
      } else {
        std::lock_guard<std::mutex> lk(ctx.mx);
        if (ctx.model) {  // 该连接已绑定设备
          pkt = pack_op_header(OP_REP_IMPORT, 1);
        } else {
          pkt = pack_rep_import(model->exported());
        }
      }
      bool ok = false;
      {
        std::lock_guard<std::mutex> lk(ctx.mx);
        ok = ctx.send_locked(pkt);
      }
      if (!ok || !model || pkt.size() == 8) {  // 失败或已占用：继续控制循环
        if (!ok) break;
        continue;
      }
      {
        std::lock_guard<std::mutex> lk(ctx.mx);
        ctx.model = model;
        ctx.sink = new ConnSink(&ctx);
      }
      model->attach_sink(ctx.sink);
      PB_LOG_INFO("usbip: imported " + busid);
      if (!serve_urb_loop(ctx)) break;
      break;
    }
    // 未知 opcode：直接断开
    PB_LOG_WARN("usbip: unknown opcode 0x" + to_hex(opb, 4));
    break;
  }

  // 清理
  std::shared_ptr<pb::usb::IUsbDeviceModel> model;
  {
    std::lock_guard<std::mutex> lk(ctx.mx);
    model = ctx.model;
    ctx.model = nullptr;
    ctx.sink = nullptr;  // 刻意不 delete：complete_in 可能仍在其他线程执行（进程生命周期内少量泄漏）
    ctx.pending_ins.clear();
    ctx.detached = true;
  }
  if (model) model->on_detach();
  {
    std::lock_guard<std::mutex> lk(conns_mx);
    for (auto it = conns.begin(); it != conns.end(); ++it)
      if (*it == &ctx.conn) { conns.erase(it); break; }
  }
  ctx.conn.close();
}

bool Server::Impl::serve_urb_loop(ConnCtx& ctx) {
  std::string err;
  while (running.load()) {
    uint8_t basic[20];
    if (!ctx.conn.recv_exact(basic, 20, &err)) return false;
    ByteReader br(basic, 20);
    uint32_t command = br.u32be();

    if (command == USBIP_CMD_SUBMIT) {
      uint8_t rest[28];
      if (!ctx.conn.recv_exact(rest, 28, &err)) return false;
      std::vector<uint8_t> full(basic, basic + 20);
      full.insert(full.end(), rest, rest + 28);
      SubmitHeader h;
      if (!parse_submit(full.data(), full.size(), h)) {
        PB_LOG_WARN("usbip: malformed CMD_SUBMIT");
        return false;
      }

      if (h.ep == 0) {
        // 控制传输
        auto setup = pb::usb::parse_setup(h.setup);
        std::vector<uint8_t> data;
        if (h.direction == DIR_OUT && h.transfer_buffer_length > 0) {
          data.resize((size_t)h.transfer_buffer_length);
          if (!ctx.conn.recv_exact(data.data(), data.size(), &err)) return false;
        }
        std::vector<uint8_t> resp;
        int32_t status = URB_STATUS_SUCCESS;
        try {
          resp = ctx.model->on_control(setup, data);
        } catch (pb::usb::ControlStall& st) {
          PB_LOG_DEBUG(std::string("usbip: control STALL ep0: ") + st.what());
          status = URB_STATUS_EPIPE;
          resp.clear();
        }
        int32_t actual = 0;
        if (status == URB_STATUS_SUCCESS) {
          if (h.direction == DIR_IN)
            actual = (int32_t)std::min<size_t>(resp.size(), (size_t)h.transfer_buffer_length);
          else
            actual = (int32_t)data.size();
        }
        reply_submit(ctx, h, status, actual,
                     h.direction == DIR_IN ? resp.data() : nullptr,
                     h.direction == DIR_IN ? (size_t)actual : 0);
        continue;
      }

      if (h.direction == DIR_OUT) {
        std::vector<uint8_t> data;
        if (h.transfer_buffer_length > 0) {
          data.resize((size_t)h.transfer_buffer_length);
          if (!ctx.conn.recv_exact(data.data(), data.size(), &err)) return false;
        }
        ctx.model->on_out((uint8_t)h.ep, std::move(data));
        reply_submit(ctx, h, URB_STATUS_SUCCESS, h.transfer_buffer_length, nullptr, 0);
        continue;
      }

      // IN：挂起，等 complete_in
      {
        std::lock_guard<std::mutex> lk(ctx.mx);
        ctx.pending_ins[(uint8_t)h.ep] = PendingIn{h.seqnum, h.devid};
      }
      ctx.model->on_in((uint8_t)h.ep);
      continue;
    }

    if (command == USBIP_CMD_UNLINK) {
      uint8_t rest[4];
      if (!ctx.conn.recv_exact(rest, 4, &err)) return false;
      ByteReader rr(basic, 20);
      rr.u32be();
      uint32_t seqnum = rr.u32be();
      uint32_t devid = rr.u32be();
      rr.u32be();  // direction
      uint32_t ep = rr.u32be();
      uint32_t unlink_seqnum = (uint32_t(rest[0]) << 24) | (uint32_t(rest[1]) << 16) |
                               (uint32_t(rest[2]) << 8) | rest[3];
      int32_t status = -1;  // 未找到挂起 URB
      {
        std::lock_guard<std::mutex> lk(ctx.mx);
        for (auto it = ctx.pending_ins.begin(); it != ctx.pending_ins.end(); ++it) {
          if (it->second.seqnum == unlink_seqnum) {
            ctx.pending_ins.erase(it);
            status = 0;
            break;
          }
        }
        ctx.send_locked(pack_ret_unlink(seqnum, devid, (uint32_t)ep, status));
      }
      continue;
    }

    PB_LOG_WARN("usbip: unknown URB opcode " + std::to_string(command));
    return false;
  }
  return false;
}

void Server::Impl::reply_submit(ConnCtx& ctx, const SubmitHeader& h, int32_t status, int32_t actual,
                                const uint8_t* data, size_t data_len) {
  auto pkt = pack_ret_submit(h.seqnum, h.devid, h.ep, h.direction, status, actual, data, data_len);
  std::lock_guard<std::mutex> lk(ctx.mx);
  ctx.send_locked(pkt);
}

void Server::Impl::ConnSink::complete_in(uint8_t ep, int32_t status, std::vector<uint8_t> data) {
  ConnCtx& ctx = *c_;
  if (ctx.detached.load()) return;
  std::lock_guard<std::mutex> lk(ctx.mx);
  auto it = ctx.pending_ins.find(ep);
  if (it == ctx.pending_ins.end()) {
    PB_LOG_DEBUG("usbip: complete_in without pending URB on ep 0x" + to_hex(&ep, 1) + " (len " +
                 std::to_string(data.size()) + ")");
    return;
  }
  PendingIn p = it->second;
  ctx.pending_ins.erase(it);
  ctx.send_locked(pack_ret_submit(p.seqnum, p.devid, ep, DIR_IN, status, (int32_t)data.size(),
                                  data.data(), data.size()));
}

void Server::Impl::ConnSink::stall_ep(uint8_t ep) {
  ConnCtx& ctx = *c_;
  std::lock_guard<std::mutex> lk(ctx.mx);
  auto it = ctx.pending_ins.find(ep);
  if (it == ctx.pending_ins.end()) return;
  PendingIn p = it->second;
  ctx.pending_ins.erase(it);
  ctx.send_locked(pack_ret_submit(p.seqnum, p.devid, ep, DIR_IN, URB_STATUS_EPIPE, 0, nullptr, 0));
}

}  // namespace pb::usbip
