#include "hci/link.h"

namespace pb::hci {

uint16_t LinkTable::alloc(uint64_t peer, uint8_t addr_type) {
  uint16_t h = next_++;
  Link l;
  l.handle = h;
  l.peer_addr = peer;
  l.peer_addr_type = addr_type;
  l.connected = true;
  links_[h] = l;
  return h;
}

Link* LinkTable::find(uint16_t handle) {
  auto it = links_.find(handle);
  return it == links_.end() ? nullptr : &it->second;
}

Link* LinkTable::find_by_peer(uint64_t addr) {
  for (auto& [h, l] : links_)
    if (l.peer_addr == addr) return &l;
  return nullptr;
}

void LinkTable::release(uint16_t handle) { links_.erase(handle); }

void LinkTable::post_to_host(uint16_t handle, const std::vector<uint8_t>& l2cap_pdu) {
  // 切片由 MiniController 侧统一执行（经 pop_acl_to_host 出队）
  for (auto& f : fragment_to_acl(handle, l2cap_pdu)) {
    pending_out_.push_back(std::move(f));
  }
}

std::vector<AclFrame> LinkTable::take_out() {
  std::vector<AclFrame> out;
  out.swap(pending_out_);
  return out;
}

std::optional<std::vector<uint8_t>> AclReassembler::feed(const AclFrame& f) {
  if (f.pb == ACL_PB_START || f.pb == ACL_PB_START_AUTO) {
    if (f.data.size() < 4) {
      reset();
      return std::nullopt;
    }
    // L2CAP 头：[len LE u16][CID LE u16]
    uint16_t len = (uint16_t)(f.data[0] | (f.data[1] << 8));
    buf_.assign(f.data.begin(), f.data.end());
    in_progress_ = true;
    if (buf_.size() - 4 >= len) {
      auto out = buf_;
      out.resize(4 + len);
      reset();
      return out;
    }
    return std::nullopt;
  }
  if (f.pb == ACL_PB_CONT && in_progress_) {
    buf_.insert(buf_.end(), f.data.begin(), f.data.end());
    if (buf_.size() < 4) return std::nullopt;
    uint16_t len = (uint16_t)(buf_[0] | (buf_[1] << 8));
    if (buf_.size() - 4 >= len) {
      auto out = buf_;
      out.resize(4 + len);
      reset();
      return out;
    }
  }
  return std::nullopt;
}

void AclReassembler::reset() {
  buf_.clear();
  in_progress_ = false;
}

std::vector<AclFrame> fragment_to_acl(uint16_t handle, const std::vector<uint8_t>& l2cap_pdu,
                                      size_t max_frag) {
  std::vector<AclFrame> out;
  if (l2cap_pdu.empty()) return out;
  size_t off = 0;
  bool first = true;
  while (off < l2cap_pdu.size()) {
    size_t n = std::min(max_frag, l2cap_pdu.size() - off);
    AclFrame f;
    f.handle = handle;
    f.pb = first ? ACL_PB_START_AUTO : ACL_PB_CONT;
    f.data.assign(l2cap_pdu.begin() + off, l2cap_pdu.begin() + off + n);
    out.push_back(std::move(f));
    off += n;
    first = false;
  }
  return out;
}

}  // namespace pb::hci
