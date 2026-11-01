#include "usbip/server.h"

#include <atomic>
#include <deque>
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
  // 连接线程登记表：stop 时关闭 socket 解除阻塞 + join 等待线程退出
  //（防止孤儿线程访问已析构的 Registry/Simulator —— UAF 根治）
  struct ConnCtx;  // Impl 作用域前置声明
  struct ConnEntry {
    std::thread th;
    ConnCtx* ctx = nullptr;  // 线程退出后置空
  };
  std::mutex conns_mx;
  std::vector<ConnEntry*> conns;
  uint16_t bound_port = 0;
  // 单导出单导入：当前已被某连接导入的设备（import 成功登记，连接清理时释放）。
  // 没有它，第二个连接 import 同一设备会 attach_sink 覆盖首个连接的 sink，
  // 首个连接的挂起 IN 永远等不到完成。
  std::mutex imports_mx;
  std::set<std::string> imported_busids;

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
    std::mutex mx;                                            // 保护 socket 写 + pending_ins
    std::map<uint8_t, std::deque<PendingIn>> pending_ins;     // ep → 挂起 IN URB 队列（FIFO，支持主机多 URB 排队）
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
  void handle_connection(net::TcpConn conn, ConnEntry* entry);
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
  // 先 join accept 线程：此后 conns 不再新增（accept_loop 是唯一登记方）
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
  // join 所有连接线程后再 delete ConnEntry：线程退出路径仍会写 entry->ctx
  //（handle_connection 尾部），先 delete 再 join 是 use-after-free
  std::vector<Server::Impl::ConnEntry*> entries;
  std::vector<std::thread> to_join;
  {
    std::lock_guard<std::mutex> lk(impl_->conns_mx);
    for (auto* e : impl_->conns) {
      if (e->ctx) e->ctx->conn.close();  // 解除阻塞读
      to_join.push_back(std::move(e->th));
      entries.push_back(e);
    }
    impl_->conns.clear();
  }
  for (auto& t : to_join)
    if (t.joinable()) t.join();
  for (auto* e : entries) delete e;
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
    ConnEntry* entry = new ConnEntry;
    entry->ctx = nullptr;  // handle_connection 内注册
    // 登记 + 起线程在同一临界区：否则 stop() 清空 conns 后本线程才登记，
    // 该线程永不 join 且随后触碰已析构的 Impl
    bool started = false;
    {
      std::lock_guard<std::mutex> lk(conns_mx);
      if (running.load()) {
        conns.push_back(entry);
        entry->th = std::thread([this, entry, c = std::move(conn)]() mutable {
          handle_connection(std::move(c), entry);
        });
        started = true;
      }
    }
    if (!started) {
      delete entry;  // stop() 并发到达：放弃该连接（conn 析构时关闭 socket）
      break;
    }
  }
}

void Server::Impl::handle_connection(net::TcpConn conn, ConnEntry* entry) {
  ConnCtx ctx;
  ctx.conn = std::move(conn);
  {
    std::lock_guard<std::mutex> lk(conns_mx);
    entry->ctx = &ctx;  // 登记（stop 可关闭 socket 解除阻塞）
    // stop() 已扫过登记表（ctx 尚未登记、socket 未被其关闭）时须自行解除
    // 阻塞，否则 stop() 的 join 会永久悬挂
    if (!running.load()) ctx.conn.close();
  }

  std::string err;
  std::string claimed_busid;  // 本连接成功占用的设备（单导出单导入，清理时释放）
  while (running.load()) {
    // op_common：u16 version + u16 code + u32 status（BE），真实 IMPORT 请求共 8+32=40B
    uint8_t opb[8];
    if (!ctx.conn.recv_exact(opb, 8, &err)) break;
    uint16_t version = (uint16_t)((uint16_t(opb[0]) << 8) | opb[1]);
    uint16_t code = (uint16_t)((uint16_t(opb[2]) << 8) | opb[3]);
    if (version != OP_VERSION) {
      PB_LOG_WARN("usbip: unsupported op version 0x" + to_hex(opb, 2));
      break;
    }

    if (code == OP_REQ_DEVLIST) {
      auto pkt = pack_rep_devlist(reg.list());
      std::lock_guard<std::mutex> lk(ctx.mx);
      if (!ctx.send_locked(pkt)) break;
      continue;
    }
    if (code == OP_REQ_IMPORT) {
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
          // 单导出单导入：已被其他连接导入 → 错误应答，不覆盖其 sink
          std::lock_guard<std::mutex> ik(imports_mx);
          if (imported_busids.insert(busid).second) {
            claimed_busid = busid;
            pkt = pack_rep_import(model->exported());
          } else {
            PB_LOG_WARN("usbip: busid " + busid + " already imported by another connection");
            pkt = pack_op_header(OP_REP_IMPORT, 1);
          }
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
    PB_LOG_WARN("usbip: unknown opcode 0x" + to_hex(opb + 2, 2));
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
  // 释放单导出单导入占用（先于 on_detach：观测到 on_detach 即可确定占用已释放）
  if (!claimed_busid.empty()) {
    std::lock_guard<std::mutex> ik(imports_mx);
    imported_busids.erase(claimed_busid);
  }
  if (model) model->on_detach();
  // 线程退出：解除登记（entry 对象由 stop() 回收；自然退出时 ctx 置空避免悬垂）
  {
    std::lock_guard<std::mutex> lk(conns_mx);
    entry->ctx = nullptr;
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
      // resize-clamp：transfer_buffer_length 由对端完全控制，超限值不得进入
      // data.resize / 数据阶段接收路径（单包巨额分配可打挂进程）；直接回
      // EINVAL 并继续服务同一连接
      if (h.transfer_buffer_length > MAX_TRANSFER_BUFFER) {
        PB_LOG_WARN("usbip: CMD_SUBMIT transfer_buffer_length " +
                    std::to_string(h.transfer_buffer_length) + " over " +
                    std::to_string(MAX_TRANSFER_BUFFER) + "; rejected");
        reply_submit(ctx, h, URB_STATUS_EINVAL, 0, nullptr, 0);
        continue;
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
        // wire ep 为 0–15 端点号；OUT 端点地址 = 端点号（无方向位）
        ctx.model->on_out((uint8_t)h.ep, std::move(data));
        reply_submit(ctx, h, URB_STATUS_SUCCESS, h.transfer_buffer_length, nullptr, 0);
        continue;
      }

      // IN：挂起，等 complete_in。
      // usbip 协议 wire 上的 ep 是 0–15 端点号（usbip-win2 proto.cpp: usb_endpoint_num()），
      // 而模型接口按含方向位的端点地址（INT IN 0x81 / BULK IN 0x82）分发 —— 此处转换。
      {
        uint8_t ep_addr = (uint8_t)(h.ep | 0x80);
        {
          std::lock_guard<std::mutex> lk(ctx.mx);
          ctx.pending_ins[ep_addr].push_back(PendingIn{h.seqnum, h.devid});
        }
        // on_in 可能立即回调 complete_in（拿 ctx.mx），须在锁外调用
        ctx.model->on_in(ep_addr);
      }
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
        for (auto qit = ctx.pending_ins.begin(); qit != ctx.pending_ins.end(); ++qit) {
          auto& q = qit->second;
          for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->seqnum == unlink_seqnum) {
              q.erase(it);
              status = 0;
              break;
            }
          }
          if (q.empty()) ctx.pending_ins.erase(qit);
          if (status == 0) break;
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
  if (it == ctx.pending_ins.end() || it->second.empty()) {
    PB_LOG_DEBUG("usbip: complete_in without pending URB on ep 0x" + to_hex(&ep, 1) + " (len " +
                 std::to_string(data.size()) + ")");
    return;
  }
  PendingIn p = it->second.front();
  it->second.pop_front();
  if (it->second.empty()) ctx.pending_ins.erase(it);
  ctx.send_locked(pack_ret_submit(p.seqnum, p.devid, ep, DIR_IN, status, (int32_t)data.size(),
                                  data.data(), data.size()));
}

void Server::Impl::ConnSink::stall_ep(uint8_t ep) {
  ConnCtx& ctx = *c_;
  std::lock_guard<std::mutex> lk(ctx.mx);
  auto it = ctx.pending_ins.find(ep);
  if (it == ctx.pending_ins.end() || it->second.empty()) return;
  PendingIn p = it->second.front();
  it->second.pop_front();
  if (it->second.empty()) ctx.pending_ins.erase(it);
  ctx.send_locked(pack_ret_submit(p.seqnum, p.devid, ep, DIR_IN, URB_STATUS_EPIPE, 0, nullptr, 0));
}

}  // namespace pb::usbip
