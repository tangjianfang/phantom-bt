#pragma once
// LE 链路数据面：连接句柄表 + ACL 分片重组 + 双向桥
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "hci/packets.h"

namespace pb::hci {

struct Link {
  uint16_t handle = 0;
  uint64_t peer_addr = 0;
  uint8_t peer_addr_type = 0;
  bool connected = false;
  bool encrypted = false;
};

class LinkTable {
 public:
  uint16_t alloc(uint64_t peer, uint8_t addr_type);  // 0x0001 起递增
  Link* find(uint16_t handle);
  Link* find_by_peer(uint64_t addr);
  void release(uint16_t handle);
  size_t size() const { return links_.size(); }

  // 主机→设备：完整 L2CAP PDU（已按 PB 位重组）交付
  std::function<void(uint16_t handle, const std::vector<uint8_t>& l2cap_pdu)> on_host_to_device;
  // 设备→主机：入队一个完整 L2CAP PDU（内部自动 ACL 切片）
  void post_to_host(uint16_t handle, const std::vector<uint8_t>& l2cap_pdu);
  // 取走全部待发 ACL 帧（MiniController 注入 acl_to_host 队列）
  std::vector<AclFrame> take_out();

 private:
  std::map<uint16_t, Link> links_;
  uint16_t next_ = 0x0001;
  std::vector<AclFrame> pending_out_;
};

// 单连接重组器：消费主机方向 ACL 帧流，拼出完整 L2CAP PDU
class AclReassembler {
 public:
  // pb: ACL PB 标志；返回 nullopt 未完成，否则完整 PDU
  std::optional<std::vector<uint8_t>> feed(const AclFrame& f);
  void reset();

 private:
  std::vector<uint8_t> buf_;
  bool in_progress_ = false;
};

// 设备→主机方向：按 max_frag 切片（返回 ACL 帧序列）
std::vector<AclFrame> fragment_to_acl(uint16_t handle, const std::vector<uint8_t>& l2cap_pdu,
                                      size_t max_frag = 247);

}  // namespace pb::hci
