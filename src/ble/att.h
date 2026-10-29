#pragma once
// ATT 服务器（LE 固定通道 CID 0x0004 之上的 PDU 处理）
#include <cstdint>
#include <optional>
#include <vector>

#include "ble/gatt_db.h"

namespace pb::ble {

// ATT PDU 码
constexpr uint8_t ATT_ERROR = 0x01;
constexpr uint8_t ATT_MTU_REQ = 0x02, ATT_MTU_RSP = 0x03;
constexpr uint8_t ATT_FIND_INFO_REQ = 0x04, ATT_FIND_INFO_RSP = 0x05;
constexpr uint8_t ATT_FIND_BY_TYPE_REQ = 0x06, ATT_FIND_BY_TYPE_RSP = 0x07;
constexpr uint8_t ATT_READ_BY_TYPE_REQ = 0x08, ATT_READ_BY_TYPE_RSP = 0x09;
constexpr uint8_t ATT_READ_REQ = 0x0A, ATT_READ_RSP = 0x0B;
constexpr uint8_t ATT_READ_BLOB_REQ = 0x0C, ATT_READ_BLOB_RSP = 0x0D;
constexpr uint8_t ATT_READ_MULTI_REQ = 0x0E, ATT_READ_MULTI_RSP = 0x0F;
constexpr uint8_t ATT_READ_BY_GROUP_REQ = 0x10, ATT_READ_BY_GROUP_RSP = 0x11;
constexpr uint8_t ATT_WRITE_REQ = 0x12, ATT_WRITE_RSP = 0x13;
constexpr uint8_t ATT_PREPARE_WRITE_REQ = 0x16, ATT_PREPARE_WRITE_RSP = 0x17;
constexpr uint8_t ATT_EXECUTE_WRITE_REQ = 0x18, ATT_EXECUTE_WRITE_RSP = 0x19;
constexpr uint8_t ATT_HANDLE_VALUE_NTF = 0x1B;
constexpr uint8_t ATT_WRITE_CMD = 0x52;

class AttServer {
 public:
  explicit AttServer(GattDb& db);
  // 处理一条 ATT PDU（无 data 前 4B L2CAP 头）；返回应答或 nullopt（WriteCmd 无应答）
  std::optional<std::vector<uint8_t>> handle(const uint8_t* pdu, size_t n);
  uint16_t peer_mtu() const { return db_.mtu; }
  // 组装通知 PDU（HOGP 报告用）
  static std::vector<uint8_t> build_notification(uint16_t handle, const std::vector<uint8_t>& value);

 private:
  std::vector<uint8_t> error(uint8_t req, uint16_t handle, uint8_t code);
  GattDb& db_;
};

}  // namespace pb::ble
