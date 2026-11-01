#include "ble/l2cap.h"

#include "core/bytes.h"

namespace pb::ble {

std::vector<uint8_t> build_l2cap(uint16_t cid, const std::vector<uint8_t>& payload) {
  ByteWriter w;
  w.u16le((uint16_t)payload.size());
  w.u16le(cid);
  w.bytes(payload.data(), payload.size());
  return w.take();
}

bool parse_l2cap(const std::vector<uint8_t>& pdu, uint16_t& cid, std::vector<uint8_t>& payload) {
  if (pdu.size() < 4) return false;
  uint16_t len = (uint16_t)(pdu[0] | (pdu[1] << 8));
  cid = (uint16_t)(pdu[2] | (pdu[3] << 8));
  if (pdu.size() - 4 < len) return false;
  payload.assign(pdu.begin() + 4, pdu.begin() + 4 + len);
  return true;
}

std::vector<uint8_t> L2capFixedDemux::handle(const std::vector<uint8_t>& pdu) {
  uint16_t cid;
  std::vector<uint8_t> payload;
  if (!parse_l2cap(pdu, cid, payload)) return {};
  // ATT Write Command / 未支持的 SMP PDU 等无应答场景返回空 —— 不得封装成
  // 4 字节零长度 L2CAP PDU（零长度 ATT/SM PDU 无 opcode，违反 Vol 3 Part A/F）
  if (cid == L2CAP_CID_ATT && on_att) {
    auto rsp = on_att(payload);
    if (rsp.empty()) return {};
    return build_l2cap(cid, std::move(rsp));
  }
  if (cid == L2CAP_CID_SM && on_sm) {
    auto rsp = on_sm(payload);
    if (rsp.empty()) return {};
    return build_l2cap(cid, std::move(rsp));
  }
  return {};
}

}  // namespace pb::ble
