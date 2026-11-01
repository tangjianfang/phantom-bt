#pragma once
// HCI 报文编解码（Bluetooth Core Spec Vol 2 Part E；多字节小端）
#include <cstdint>
#include <cstring>
#include <vector>

namespace pb::hci {

constexpr uint16_t opcode(uint16_t ogf, uint16_t ocf) { return (uint16_t)((ogf << 10) | ocf); }
constexpr uint16_t ogf_of(uint16_t op) { return (uint16_t)(op >> 10); }
constexpr uint16_t ocf_of(uint16_t op) { return (uint16_t)(op & 0x3FF); }

constexpr uint16_t OGF_LINK_CTRL = 0x01, OGF_LINK_POLICY = 0x02, OGF_HC_BASEBAND = 0x03,
                   OGF_INFO = 0x04, OGF_STATUS = 0x05, OGF_TESTING = 0x06, OGF_LE = 0x08,
                   OGF_VENDOR = 0x3F;

// —— 常用命令 opcode ——
// Baseband / Info / Status
constexpr uint16_t CMD_SET_EVENT_MASK = opcode(0x03, 0x0001);
constexpr uint16_t CMD_RESET = opcode(0x03, 0x0003);
constexpr uint16_t CMD_SET_EVENT_FILTER = opcode(0x03, 0x0005);
constexpr uint16_t CMD_WRITE_PAGE_TIMEOUT = opcode(0x03, 0x0018);
constexpr uint16_t CMD_WRITE_SCAN_ENABLE = opcode(0x03, 0x001A);
constexpr uint16_t CMD_WRITE_AUTHENTICATION_ENABLE = opcode(0x03, 0x0020);
constexpr uint16_t CMD_WRITE_CLASS_OF_DEVICE = opcode(0x03, 0x0024);
constexpr uint16_t CMD_WRITE_LOCAL_NAME = opcode(0x03, 0x0013);
constexpr uint16_t CMD_WRITE_INQUIRY_MODE = opcode(0x03, 0x0045);
constexpr uint16_t CMD_WRITE_VOICE_SETTING = opcode(0x03, 0x0026);
constexpr uint16_t CMD_WRITE_EIR = opcode(0x03, 0x0052);
constexpr uint16_t CMD_HOST_BUFFER_SIZE = opcode(0x03, 0x0033);
constexpr uint16_t CMD_HOST_NUM_COMPLETED_PACKETS = opcode(0x03, 0x0035);  // 0x0C35
constexpr uint16_t CMD_READ_INQUIRY_MODE = opcode(0x03, 0x0044);
constexpr uint16_t CMD_READ_PAGE_TIMEOUT = opcode(0x03, 0x0017);
constexpr uint16_t CMD_READ_SCAN_ENABLE = opcode(0x03, 0x0019);
constexpr uint16_t CMD_READ_AUTHENTICATION_ENABLE = opcode(0x03, 0x001F);
constexpr uint16_t CMD_READ_CLASS_OF_DEVICE = opcode(0x03, 0x0023);
constexpr uint16_t CMD_READ_VOICE_SETTING = opcode(0x03, 0x0025);
constexpr uint16_t CMD_READ_LOCAL_NAME = opcode(0x03, 0x0014);
constexpr uint16_t CMD_READ_NUM_BROADCAST = opcode(0x03, 0x0038);
constexpr uint16_t CMD_WRITE_NUM_BROADCAST = opcode(0x03, 0x003A);
constexpr uint16_t CMD_WRITE_PAGE_SCAN_ACTIVITY = opcode(0x03, 0x001C);
constexpr uint16_t CMD_WRITE_INQUIRY_SCAN_ACTIVITY = opcode(0x03, 0x001E);
constexpr uint16_t CMD_WRITE_PAGE_SCAN_TYPE = opcode(0x03, 0x0047);
constexpr uint16_t CMD_WRITE_INQUIRY_SCAN_TYPE = opcode(0x03, 0x0043);
constexpr uint16_t CMD_WRITE_SIMPLE_PAIRING_MODE = opcode(0x03, 0x0056);
constexpr uint16_t CMD_WRITE_SC_HOST_SUPPORT = opcode(0x03, 0x007A);
constexpr uint16_t CMD_READ_STORED_LINK_KEY = opcode(0x03, 0x000D);
constexpr uint16_t CMD_WRITE_STORED_LINK_KEY = opcode(0x03, 0x0011);  // 0x0C11
constexpr uint16_t CMD_DELETE_STORED_LINK_KEY = opcode(0x03, 0x0012);
constexpr uint16_t CMD_WRITE_PIN_TYPE = opcode(0x03, 0x000A);
constexpr uint16_t CMD_WRITE_LOOPBACK_MODE = opcode(0x03, 0x002D);
constexpr uint16_t CMD_READ_DEFAULT_ERRONEOUS_REPORTING = opcode(0x03, 0x005A);
constexpr uint16_t CMD_WRITE_DEFAULT_ERRONEOUS_REPORTING = opcode(0x03, 0x005B);
constexpr uint16_t CMD_READ_LE_HOST_SUPPORT = opcode(0x03, 0x006C);
constexpr uint16_t CMD_WRITE_LE_HOST_SUPPORT = opcode(0x03, 0x006D);
constexpr uint16_t CMD_READ_FLOW_CONTROL_MODE = opcode(0x03, 0x0066);
constexpr uint16_t CMD_WRITE_FLOW_CONTROL_MODE = opcode(0x03, 0x0067);
constexpr uint16_t CMD_READ_LOCAL_EXTENDED_FEATURES = opcode(0x04, 0x0004);
constexpr uint16_t CMD_READ_BUFFER_SIZE = opcode(0x04, 0x0005);
constexpr uint16_t CMD_READ_COUNTRY_CODE = opcode(0x04, 0x0007);
constexpr uint16_t CMD_READ_BD_ADDR = opcode(0x04, 0x0009);
constexpr uint16_t CMD_READ_LOCAL_VERSION = opcode(0x04, 0x0001);
constexpr uint16_t CMD_READ_LOCAL_SUPPORTED_COMMANDS = opcode(0x04, 0x0002);
constexpr uint16_t CMD_READ_LOCAL_SUPPORTED_FEATURES = opcode(0x04, 0x0003);
// LE controller 命令
constexpr uint16_t CMD_LE_SET_EVENT_MASK = opcode(0x08, 0x0001);
constexpr uint16_t CMD_LE_READ_BUFFER_SIZE = opcode(0x08, 0x0002);
constexpr uint16_t CMD_LE_READ_LOCAL_SUPPORTED_FEATURES = opcode(0x08, 0x0003);
constexpr uint16_t CMD_LE_SET_RANDOM_ADDRESS = opcode(0x08, 0x0005);
constexpr uint16_t CMD_LE_SET_ADVERTISING_PARAMETERS = opcode(0x08, 0x0006);
constexpr uint16_t CMD_LE_READ_ADVERTISING_TX_POWER = opcode(0x08, 0x0007);
constexpr uint16_t CMD_LE_SET_ADVERTISING_DATA = opcode(0x08, 0x0008);
constexpr uint16_t CMD_LE_SET_SCAN_RESPONSE_DATA = opcode(0x08, 0x0009);
constexpr uint16_t CMD_LE_SET_ADVERTISING_ENABLE = opcode(0x08, 0x000A);
constexpr uint16_t CMD_LE_SET_SCAN_PARAMETERS = opcode(0x08, 0x000B);
constexpr uint16_t CMD_LE_SET_SCAN_ENABLE = opcode(0x08, 0x000C);
constexpr uint16_t CMD_LE_CREATE_CONNECTION = opcode(0x08, 0x000D);
constexpr uint16_t CMD_LE_CREATE_CONNECTION_CANCEL = opcode(0x08, 0x000E);
constexpr uint16_t CMD_LE_READ_WHITE_LIST_SIZE = opcode(0x08, 0x000F);
constexpr uint16_t CMD_LE_CLEAR_WHITE_LIST = opcode(0x08, 0x0010);
constexpr uint16_t CMD_LE_ADD_DEVICE_TO_WHITE_LIST = opcode(0x08, 0x0011);
constexpr uint16_t CMD_LE_REMOVE_DEVICE_FROM_WHITE_LIST = opcode(0x08, 0x0012);
constexpr uint16_t CMD_LE_CONNECTION_UPDATE = opcode(0x08, 0x0013);
constexpr uint16_t CMD_LE_SET_HOST_CHANNEL_CLASSIFICATION = opcode(0x08, 0x0014);
constexpr uint16_t CMD_LE_READ_CHANNEL_MAP = opcode(0x08, 0x0015);
constexpr uint16_t CMD_LE_READ_REMOTE_FEATURES = opcode(0x08, 0x0016);
constexpr uint16_t CMD_LE_ENCRYPT = opcode(0x08, 0x0017);
constexpr uint16_t CMD_LE_RAND = opcode(0x08, 0x0018);
constexpr uint16_t CMD_LE_START_ENCRYPTION = opcode(0x08, 0x0019);
constexpr uint16_t CMD_LE_LTK_REQUEST_REPLY = opcode(0x08, 0x001A);
constexpr uint16_t CMD_LE_LTK_REQUEST_NEGATIVE_REPLY = opcode(0x08, 0x001B);
constexpr uint16_t CMD_LE_READ_SUPPORTED_STATES = opcode(0x08, 0x001C);
constexpr uint16_t CMD_CMD_DISCONNECT = opcode(0x01, 0x0006);

// —— 事件码 ——
constexpr uint8_t EVT_INQUIRY_COMPLETE = 0x01, EVT_INQUIRY_RESULT = 0x02,
                  EVT_CONNECTION_COMPLETE = 0x03, EVT_CONNECTION_REQUEST = 0x04,
                  EVT_DISCONNECTION_COMPLETE = 0x05, EVT_AUTH_COMPLETE = 0x06,
                  EVT_REMOTE_NAME_REQUEST_COMPLETE = 0x07, EVT_ENCRYPTION_CHANGE = 0x08,
                  EVT_ROLE_CHANGE = 0x12, EVT_NUMBER_COMPLETED_PKTS = 0x13,
                  EVT_MODE_CHANGE = 0x14, EVT_RETURN_LINK_KEYS = 0x15,
                  EVT_PIN_CODE_REQUEST = 0x16, EVT_LINK_KEY_REQUEST = 0x17,
                  EVT_LINK_KEY_NOTIFICATION = 0x18, EVT_LOOPBACK_COMMAND = 0x19,
                  EVT_DATA_BUFFER_OVERFLOW = 0x1A, EVT_MAX_SLOTS_CHANGE = 0x1B,
                  EVT_COMMAND_COMPLETE = 0x0E, EVT_COMMAND_STATUS = 0x0F,
                  EVT_HARDWARE_ERROR = 0x10, EVT_FLUSH_OCCURRED = 0x11,
                  EVT_QOS_SETUP_COMPLETE = 0x0D, EVT_READ_REMOTE_VERSION_COMPLETE = 0x0C,
                  EVT_READ_REMOTE_SUPPORTED_FEATURES_COMPLETE = 0x0B,
                  EVT_LINK_SUPERVISION_TIMEOUT_CHANGED = 0x38,
                  EVT_ENHANCED_FLUSH_COMPLETE = 0x39, EVT_USER_CONFIRM_REQUEST = 0x33,
                  EVT_USER_PASSKEY_REQUEST = 0x34, EVT_REMOTE_OOB_DATA_REQUEST = 0x35,
                  EVT_SIMPLE_PAIRING_COMPLETE = 0x36, EVT_LE_META = 0x3E,
                  EVT_COMMAND_STATUS_PENDING = 0x00;

// LE Meta 子事件
constexpr uint8_t LE_EVT_CONNECTION_COMPLETE = 0x01, LE_EVT_ADVERTISING_REPORT = 0x02,
                  LE_EVT_CONNECTION_UPDATE_COMPLETE = 0x03,
                  LE_EVT_READ_REMOTE_FEATURES_COMPLETE = 0x04,
                  LE_EVT_LTK_REQUEST = 0x05,
                  LE_EVT_REMOTE_CONNECTION_PARAMETER_REQUEST = 0x06,
                  LE_EVT_DATA_LENGTH_CHANGE = 0x07;

// —— 状态码 ——
constexpr uint8_t ST_SUCCESS = 0x00, ST_UNKNOWN_CMD = 0x01, ST_UNKNOWN_CONN_ID = 0x02,
                  ST_HW_FAILURE = 0x03, ST_PAGE_TIMEOUT = 0x04, ST_AUTH_FAILURE = 0x05,
                  ST_PIN_OR_KEY_MISSING = 0x06, ST_MEMORY_FULL = 0x07, ST_CONN_TIMEOUT = 0x08,
                  ST_CONN_LIMIT_EXCEEDED = 0x09, ST_SYNC_CONN_LIMIT = 0x0A,
                  ST_ACL_CONN_EXISTS = 0x0B, ST_CMD_DISALLOWED = 0x0C,
                  ST_LIMITED_RESOURCES = 0x0D, ST_SECURITY_ERROR = 0x11,
                  ST_UNSUPPORTED = 0x11 /*同上按上下文区分*/,
                  ST_INVALID_PARAMS = 0x12, ST_REMOTE_USER_TERM = 0x13,
                  ST_REMOTE_LOW_RESOURCES = 0x14, ST_UNSUPPORTED_REMOTE = 0x1A,
                  ST_UNSPECIFIED_ERROR = 0x1F,
                  // Operation Cancelled by Host：LE_Create_Connection_Cancel 终止挂起建链
                  ST_OPERATION_CANCELLED = 0x42;

// —— ACL 帧头 ——
constexpr uint16_t ACL_PB_START = 0x0000, ACL_PB_CONT = 0x1000, ACL_PB_START_AUTO = 0x2000;

struct HciCommand {
  uint16_t op = 0;
  std::vector<uint8_t> params;
};

inline bool parse_command(const uint8_t* p, size_t n, HciCommand& out) {
  if (n < 3 || (size_t)p[2] != n - 3) return false;
  out.op = (uint16_t)(p[0] | (p[1] << 8));
  out.params.assign(p + 3, p + n);
  return true;
}

inline std::vector<uint8_t> build_command(uint16_t op, const std::vector<uint8_t>& params) {
  std::vector<uint8_t> v;
  v.push_back(op & 0xFF);
  v.push_back(op >> 8);
  v.push_back((uint8_t)params.size());
  v.insert(v.end(), params.begin(), params.end());
  return v;
}

inline std::vector<uint8_t> build_event(uint8_t event, const std::vector<uint8_t>& params) {
  std::vector<uint8_t> v;
  v.push_back(event);
  v.push_back((uint8_t)params.size());
  v.insert(v.end(), params.begin(), params.end());
  return v;
}

// Command Complete: num_pkts, op LE, return_params（首字节通常 status）
inline std::vector<uint8_t> build_command_complete(uint8_t num_pkts, uint16_t op,
                                                   const std::vector<uint8_t>& ret) {
  std::vector<uint8_t> p;
  p.push_back(num_pkts);
  p.push_back(op & 0xFF);
  p.push_back(op >> 8);
  p.insert(p.end(), ret.begin(), ret.end());
  return build_event(EVT_COMMAND_COMPLETE, p);
}

// Command Status: status, num_pkts, op LE
inline std::vector<uint8_t> build_command_status(uint8_t status, uint8_t num_pkts, uint16_t op) {
  std::vector<uint8_t> p;
  p.push_back(status);
  p.push_back(num_pkts);
  p.push_back(op & 0xFF);
  p.push_back(op >> 8);
  return build_event(EVT_COMMAND_STATUS, p);
}

inline std::vector<uint8_t> build_le_meta(uint8_t subevent, const std::vector<uint8_t>& params) {
  std::vector<uint8_t> p;
  p.push_back(subevent);
  p.insert(p.end(), params.begin(), params.end());
  return build_event(EVT_LE_META, p);
}

struct AclFrame {
  uint16_t handle = 0;
  uint16_t pb = ACL_PB_START;
  std::vector<uint8_t> data;
};

inline bool parse_acl(const uint8_t* p, size_t n, AclFrame& out) {
  if (n < 4) return false;
  uint16_t hf = (uint16_t)(p[0] | (p[1] << 8));
  uint16_t len = (uint16_t)(p[2] | (p[3] << 8));
  if (n - 4 < len) return false;
  out.handle = hf & 0x0FFF;
  out.pb = hf & 0x3000;
  out.data.assign(p + 4, p + 4 + len);
  return true;
}

inline std::vector<uint8_t> build_acl(const AclFrame& f) {
  uint16_t hf = (uint16_t)((f.handle & 0x0FFF) | (f.pb & 0x3000));
  std::vector<uint8_t> v;
  v.push_back(hf & 0xFF);
  v.push_back(hf >> 8);
  v.push_back(f.data.size() & 0xFF);
  v.push_back(f.data.size() >> 8);
  v.insert(v.end(), f.data.begin(), f.data.end());
  return v;
}

}  // namespace pb::hci
