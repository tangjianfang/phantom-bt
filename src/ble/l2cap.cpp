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
  if (cid == L2CAP_CID_ATT && on_att) return build_l2cap(cid, on_att(payload));
  if (cid == L2CAP_CID_SM && on_sm) return build_l2cap(cid, on_sm(payload));
  return {};
}

}  // namespace pb::ble
