#pragma once
// L2CAP LE 固定通道分发（ATT CID 0x0004 / SM CID 0x0006）
#include <cstdint>
#include <functional>
#include <vector>

namespace pb::ble {

constexpr uint16_t L2CAP_CID_ATT = 0x0004;
constexpr uint16_t L2CAP_CID_SM = 0x0006;

// 组包：L2CAP 头（len LE + cid LE）+ payload
std::vector<uint8_t> build_l2cap(uint16_t cid, const std::vector<uint8_t>& payload);
// 解包：返回 (cid, payload)；长度不符返回 false
bool parse_l2cap(const std::vector<uint8_t>& pdu, uint16_t& cid, std::vector<uint8_t>& payload);

class L2capFixedDemux {
 public:
  // 各通道处理器（可为空 = 丢弃）
  std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> on_att;   // 返回应答
  std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> on_sm;
  // 输入完整 L2CAP PDU → 可选应答 PDU
  std::vector<uint8_t> handle(const std::vector<uint8_t>& pdu);
};

}  // namespace pb::ble
