#include "hci/controller.h"

#include <unordered_map>

#include "core/bytes.h"
#include "core/log.h"

namespace pb::hci {

namespace {

constexpr uint16_t kConnDelayTicks = 13;  // tick(16ms) × 13 ≈ 208ms 连接建立延迟

// 常用命令命名（日志用；不要求全覆盖）
const std::unordered_map<uint16_t, const char*>& command_names() {
  static const std::unordered_map<uint16_t, const char*> m = {
      {CMD_RESET, "Reset"},
      {CMD_SET_EVENT_MASK, "Set_Event_Mask"},
      {CMD_SET_EVENT_FILTER, "Set_Event_Filter"},
      {CMD_WRITE_PAGE_TIMEOUT, "Write_Page_Timeout"},
      {CMD_WRITE_SCAN_ENABLE, "Write_Scan_Enable"},
      {CMD_WRITE_AUTHENTICATION_ENABLE, "Write_Authentication_Enable"},
      {CMD_WRITE_CLASS_OF_DEVICE, "Write_Class_Of_Device"},
      {CMD_WRITE_LOCAL_NAME, "Write_Local_Name"},
      {CMD_READ_LOCAL_NAME, "Read_Local_Name"},
      {CMD_WRITE_INQUIRY_MODE, "Write_Inquiry_Mode"},
      {CMD_READ_INQUIRY_MODE, "Read_Inquiry_Mode"},
      {CMD_WRITE_VOICE_SETTING, "Write_Voice_Setting"},
      {CMD_READ_VOICE_SETTING, "Read_Voice_Setting"},
      {CMD_WRITE_EIR, "Write_Extended_Inquiry_Response"},
      {CMD_HOST_BUFFER_SIZE, "Host_Buffer_Size"},
      {CMD_HOST_NUM_COMPLETED_PACKETS, "Host_Number_Of_Completed_Packets"},
      {CMD_READ_BUFFER_SIZE, "Read_Buffer_Size"},
      {CMD_READ_COUNTRY_CODE, "Read_Country_Code"},
      {CMD_READ_BD_ADDR, "Read_BD_ADDR"},
      {CMD_READ_LOCAL_VERSION, "Read_Local_Version_Information"},
      {CMD_READ_LOCAL_SUPPORTED_COMMANDS, "Read_Local_Supported_Commands"},
      {CMD_READ_LOCAL_SUPPORTED_FEATURES, "Read_Local_Supported_Features"},
      {CMD_READ_LOCAL_EXTENDED_FEATURES, "Read_Local_Extended_Features"},
      {CMD_WRITE_SIMPLE_PAIRING_MODE, "Write_Simple_Pairing_Mode"},
      {CMD_WRITE_SC_HOST_SUPPORT, "Write_Secure_Connections_Host_Support"},
      {CMD_WRITE_LE_HOST_SUPPORT, "LE_Host_Support"},
      {CMD_READ_LE_HOST_SUPPORT, "Read_LE_Host_Support"},
      {CMD_LE_SET_EVENT_MASK, "LE_Set_Event_Mask"},
      {CMD_LE_READ_BUFFER_SIZE, "LE_Read_Buffer_Size"},
      {CMD_LE_READ_LOCAL_SUPPORTED_FEATURES, "LE_Read_Local_Supported_Features"},
      {CMD_LE_SET_RANDOM_ADDRESS, "LE_Set_Random_Address"},
      {CMD_LE_SET_ADVERTISING_PARAMETERS, "LE_Set_Advertising_Parameters"},
      {CMD_LE_READ_ADVERTISING_TX_POWER, "LE_Read_Advertising_Tx_Power"},
      {CMD_LE_SET_ADVERTISING_DATA, "LE_Set_Advertising_Data"},
      {CMD_LE_SET_SCAN_RESPONSE_DATA, "LE_Set_Scan_Response_Data"},
      {CMD_LE_SET_ADVERTISING_ENABLE, "LE_Set_Advertising_Enable"},
      {CMD_LE_SET_SCAN_PARAMETERS, "LE_Set_Scan_Parameters"},
      {CMD_LE_SET_SCAN_ENABLE, "LE_Set_Scan_Enable"},
      {CMD_LE_CREATE_CONNECTION, "LE_Create_Connection"},
      {CMD_LE_CREATE_CONNECTION_CANCEL, "LE_Create_Connection_Cancel"},
      {CMD_LE_READ_WHITE_LIST_SIZE, "LE_Read_White_List_Size"},
      {CMD_LE_CLEAR_WHITE_LIST, "LE_Clear_White_List"},
      {CMD_LE_SET_HOST_CHANNEL_CLASSIFICATION, "LE_Set_Host_Channel_Classification"},
      {CMD_LE_READ_CHANNEL_MAP, "LE_Read_Channel_Map"},
      {CMD_LE_READ_REMOTE_FEATURES, "LE_Read_Remote_Features"},
      {CMD_LE_ENCRYPT, "LE_Encrypt"},
      {CMD_LE_RAND, "LE_Rand"},
      {CMD_LE_START_ENCRYPTION, "LE_Start_Encryption"},
      {CMD_LE_LTK_REQUEST_REPLY, "LE_Long_Term_Key_Request_Reply"},
      {CMD_LE_READ_SUPPORTED_STATES, "LE_Read_Supported_States"},
      {CMD_CMD_DISCONNECT, "Disconnect"},
      {CMD_WRITE_PAGE_SCAN_ACTIVITY, "Write_Page_Scan_Activity"},
      {CMD_WRITE_INQUIRY_SCAN_ACTIVITY, "Write_Inquiry_Scan_Activity"},
      {CMD_WRITE_PAGE_SCAN_TYPE, "Write_Page_Scan_Type"},
      {CMD_WRITE_INQUIRY_SCAN_TYPE, "Write_Inquiry_Scan_Type"},
      {CMD_DELETE_STORED_LINK_KEY, "Delete_Stored_Link_Key"},
      {CMD_READ_STORED_LINK_KEY, "Read_Stored_Link_Key"},
      {CMD_WRITE_PIN_TYPE, "Write_PIN_Type"},
      {CMD_READ_NUM_BROADCAST, "Read_Num_Broadcast_Retransmissions"},
      {CMD_WRITE_NUM_BROADCAST, "Write_Num_Broadcast_Retransmissions"},
      {CMD_READ_DEFAULT_ERRONEOUS_REPORTING, "Read_Default_Erroneous_Data_Reporting"},
      {CMD_WRITE_DEFAULT_ERRONEOUS_REPORTING, "Write_Default_Erroneous_Data_Reporting"},
      {CMD_READ_FLOW_CONTROL_MODE, "Read_Flow_Control_Mode"},
      {CMD_WRITE_FLOW_CONTROL_MODE, "Write_Flow_Control_Mode"},
  };
  return m;
}

}  // namespace

size_t supported_commands_bit(uint16_t op) {
  uint16_t ogf = ogf_of(op), ocf = ocf_of(op);
  return (size_t)((ogf - 1) * 64 + (ocf & 0x3F));
}

const std::vector<uint16_t>& supported_command_list() {
  static const std::vector<uint16_t> list = {
      CMD_RESET,
      CMD_SET_EVENT_MASK,
      CMD_SET_EVENT_FILTER,
      CMD_WRITE_PAGE_TIMEOUT,
      CMD_WRITE_SCAN_ENABLE,
      CMD_READ_SCAN_ENABLE,
      CMD_WRITE_AUTHENTICATION_ENABLE,
      CMD_READ_AUTHENTICATION_ENABLE,
      CMD_WRITE_CLASS_OF_DEVICE,
      CMD_READ_CLASS_OF_DEVICE,
      CMD_WRITE_LOCAL_NAME,
      CMD_READ_LOCAL_NAME,
      CMD_WRITE_INQUIRY_MODE,
      CMD_READ_INQUIRY_MODE,
      CMD_WRITE_VOICE_SETTING,
      CMD_READ_VOICE_SETTING,
      CMD_WRITE_EIR,
      CMD_HOST_BUFFER_SIZE,
      CMD_HOST_NUM_COMPLETED_PACKETS,
      CMD_READ_BUFFER_SIZE,
      CMD_READ_COUNTRY_CODE,
      CMD_READ_BD_ADDR,
      CMD_READ_LOCAL_VERSION,
      CMD_READ_LOCAL_SUPPORTED_COMMANDS,
      CMD_READ_LOCAL_SUPPORTED_FEATURES,
      CMD_READ_LOCAL_EXTENDED_FEATURES,
      CMD_WRITE_SIMPLE_PAIRING_MODE,
      CMD_WRITE_SC_HOST_SUPPORT,
      CMD_READ_LE_HOST_SUPPORT,
      CMD_WRITE_LE_HOST_SUPPORT,
      CMD_WRITE_PAGE_SCAN_ACTIVITY,
      CMD_WRITE_INQUIRY_SCAN_ACTIVITY,
      CMD_WRITE_PAGE_SCAN_TYPE,
      CMD_WRITE_INQUIRY_SCAN_TYPE,
      CMD_DELETE_STORED_LINK_KEY,
      CMD_READ_STORED_LINK_KEY,
      CMD_WRITE_PIN_TYPE,
      CMD_READ_NUM_BROADCAST,
      CMD_WRITE_NUM_BROADCAST,
      CMD_READ_DEFAULT_ERRONEOUS_REPORTING,
      CMD_WRITE_DEFAULT_ERRONEOUS_REPORTING,
      CMD_READ_FLOW_CONTROL_MODE,
      CMD_WRITE_FLOW_CONTROL_MODE,
      // LE
      CMD_LE_SET_EVENT_MASK,
      CMD_LE_READ_BUFFER_SIZE,
      CMD_LE_READ_LOCAL_SUPPORTED_FEATURES,
      CMD_LE_SET_RANDOM_ADDRESS,
      CMD_LE_SET_ADVERTISING_PARAMETERS,
      CMD_LE_READ_ADVERTISING_TX_POWER,
      CMD_LE_SET_ADVERTISING_DATA,
      CMD_LE_SET_SCAN_RESPONSE_DATA,
      CMD_LE_SET_ADVERTISING_ENABLE,
      CMD_LE_SET_SCAN_PARAMETERS,
      CMD_LE_SET_SCAN_ENABLE,
      CMD_LE_CREATE_CONNECTION,
      CMD_LE_CREATE_CONNECTION_CANCEL,
      CMD_LE_READ_WHITE_LIST_SIZE,
      CMD_LE_CLEAR_WHITE_LIST,
      CMD_LE_ADD_DEVICE_TO_WHITE_LIST,
      CMD_LE_REMOVE_DEVICE_FROM_WHITE_LIST,
      CMD_LE_CONNECTION_UPDATE,
      CMD_LE_SET_HOST_CHANNEL_CLASSIFICATION,
      CMD_LE_READ_CHANNEL_MAP,
      CMD_LE_READ_REMOTE_FEATURES,
      CMD_LE_ENCRYPT,
      CMD_LE_RAND,
      CMD_LE_START_ENCRYPTION,
      CMD_LE_LTK_REQUEST_REPLY,
      CMD_LE_LTK_REQUEST_NEGATIVE_REPLY,
      CMD_LE_READ_SUPPORTED_STATES,
      CMD_CMD_DISCONNECT,
  };
  return list;
}

struct MiniController::Impl {
  // 在 Supported_Commands 位图中置位本引擎支持的命令
  std::array<uint8_t, 64> commands_bitmap{};  // 逻辑上是 64B
  std::array<uint8_t, 8> br_features{0xFF, 0x8F, 0x9F, 0xFE, 0xDB, 0xFF, 0x79, 0x83};
  std::array<uint8_t, 8> le_features{0x01, 0x0F, 0, 0, 0, 0, 0, 0};
  uint16_t current_opcode = 0;
};

MiniController::MiniController(const ControllerConfig& cfg) : cfg_(cfg), impl_(new Impl) {
  local_name_stored_ = cfg.local_name;
  for (uint16_t op : supported_command_list()) {
    size_t pos = supported_commands_bit(op);
    if (pos / 8 < impl_->commands_bitmap.size()) impl_->commands_bitmap[pos / 8] |= (1 << (pos % 8));
  }
}

MiniController::~MiniController() = default;

void MiniController::push_event(const std::vector<uint8_t>& evt) {
  {
    std::lock_guard<std::mutex> lk(mx_);
    event_q_.push_back(evt);
    ++event_count_;
  }
  if (snoop_event) snoop_event(evt);
}

bool MiniController::pop_event(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lk(mx_);
  if (event_q_.empty()) return false;
  out = std::move(event_q_.front());
  event_q_.pop_front();
  return true;
}

bool MiniController::pop_acl_to_host(AclFrame& out) {
  std::lock_guard<std::mutex> lk(mx_);
  if (acl_to_host_q_.empty()) return false;
  out = std::move(acl_to_host_q_.front());
  acl_to_host_q_.pop_front();
  return true;
}

std::vector<std::string> MiniController::op_log() const {
  std::lock_guard<std::mutex> lk(mx_);
  return op_log_;
}

void MiniController::handle_acl_from_host(const AclFrame& f) {
  if (snoop_acl_in) snoop_acl_in(f);
  // 重组 → 完整 L2CAP PDU → 上层回调（ATT/SM）
  auto pdu = reasm_.feed(f);
  if (pdu && on_l2cap_from_host) {
    on_l2cap_from_host(f.handle, *pdu);
  } else if (pdu) {
    PB_LOG_DEBUG("hci: L2CAP pdu (no handler) len=" + std::to_string(pdu->size()));
  }
}

void MiniController::send_l2cap(uint16_t handle, const std::vector<uint8_t>& pdu) {
  std::lock_guard<std::mutex> lk(mx_);
  for (auto& fr : fragment_to_acl(handle, pdu)) {
    acl_to_host_q_.push_back(std::move(fr));
  }
}

void MiniController::handle_command(const uint8_t* p, size_t n) {
  if (snoop_command) snoop_command(std::vector<uint8_t>(p, p + n));
  HciCommand cmd;
  if (!parse_command(p, n, cmd)) {
    PB_LOG_WARN("hci: malformed command packet");
    return;
  }

  std::vector<uint8_t> ret;      // CC return params
  uint8_t status = ST_SUCCESS;
  bool known = true;
  const uint8_t* q = cmd.params.data();
  size_t qn = cmd.params.size();

  switch (cmd.op) {
    case CMD_RESET:
      seen_reset_ = true;
      radio_ready_ = seen_reset_ && seen_bd_addr_;
      break;
    case CMD_READ_BD_ADDR: {
      seen_bd_addr_ = true;
      radio_ready_ = seen_reset_ && seen_bd_addr_;
      for (int i = 0; i < 6; ++i) ret.push_back(cfg_.bd_addr[i]);
      break;
    }
    case CMD_READ_LOCAL_NAME: {
      std::vector<uint8_t> name(248, 0);
      size_t len = std::min<size_t>(local_name_stored_.size(), 247);
      std::memcpy(name.data(), local_name_stored_.data(), len);
      ret.assign(name.begin(), name.end());
      break;
    }
    case CMD_WRITE_LOCAL_NAME: {
      if (qn >= 248) local_name_stored_.assign((const char*)q, strnlen((const char*)q, 248));
      break;
    }
    case CMD_READ_BUFFER_SIZE: {
      ByteWriter w;
      w.u16le(cfg_.acl_mtu);
      w.u8(0);   // SCO mtu（无 SCO）
      w.u16le(0);  // SCO packets
      w.u16le(cfg_.num_acl_packets);
      auto b = w.take();
      ret.assign(b.begin(), b.end());
      break;
    }
    case CMD_READ_LOCAL_VERSION: {
      ByteWriter w;
      w.u8(cfg_.hci_version);
      w.u16le(cfg_.hci_revision);
      w.u8(cfg_.lmp_version);
      w.u16le(cfg_.manufacturer);
      w.u16le(cfg_.lmp_subversion);
      auto b = w.take();
      ret.assign(b.begin(), b.end());
      break;
    }
    case CMD_READ_LOCAL_SUPPORTED_COMMANDS: {
      ret.assign(impl_->commands_bitmap.begin(), impl_->commands_bitmap.end());
      break;
    }
    case CMD_READ_LOCAL_SUPPORTED_FEATURES: {
      ret.assign(impl_->br_features.begin(), impl_->br_features.end());
      break;
    }
    case CMD_READ_LOCAL_EXTENDED_FEATURES: {
      uint8_t page = qn >= 1 ? q[0] : 0;
      ret = {page, 1};
      if (page == 0)
        ret.insert(ret.end(), impl_->br_features.begin(), impl_->br_features.end());
      else
        ret.insert(ret.end(), 8, 0);
      break;
    }
    case CMD_READ_COUNTRY_CODE:
      ret = {0x00};
      break;
    case CMD_WRITE_EIR: {
      if (qn >= 240) std::memcpy(eir_.data(), q, 8);  // 仅存首块（诊断）
      break;
    }
    case CMD_LE_READ_BUFFER_SIZE: {
      ByteWriter w;
      w.u16le(cfg_.le_acl_mtu);
      w.u8(0);                 // LE SCO/ISO mtu = 0
      w.u8(cfg_.le_num_packets);
      auto b = w.take();
      ret.assign(b.begin(), b.end());
      break;
    }
    case CMD_LE_READ_LOCAL_SUPPORTED_FEATURES: {
      ret.assign(impl_->le_features.begin(), impl_->le_features.end());
      break;
    }
    case CMD_LE_READ_SUPPORTED_STATES: {
      // bit0: 可广播+可扫描；bit2: 可发起连接（状态图第 1/3 态保守组合）
      ret = {0x05, 0, 0, 0, 0, 0, 0, 0};
      break;
    }
    case CMD_LE_READ_WHITE_LIST_SIZE:
      ret = {0x10};
      break;
    case CMD_LE_READ_ADVERTISING_TX_POWER:
      ret = {0x00, 0x00};  // status, tx power 0dBm
      break;
    case CMD_LE_SET_SCAN_ENABLE: {
      // params: enable, filter_dup
      if (qn >= 1) {
        le_scan_enabled_ = q[0] != 0;
        air_.set_host_scanning(le_scan_enabled_, le_scan_active_);
      }
      break;
    }
    case CMD_READ_SCAN_ENABLE:
      ret = {scan_enable_};
      break;
    case CMD_WRITE_SCAN_ENABLE:
      if (qn >= 1) scan_enable_ = q[0];
      break;
    case CMD_READ_AUTHENTICATION_ENABLE:
      ret = {0x00};
      break;
    case CMD_READ_CLASS_OF_DEVICE: {
      ret = {0x00, 0x00, 0x00};  // CoD LE-bit 位由 Windows 侧自行判定
      break;
    }
    case CMD_READ_INQUIRY_MODE:
      ret = {0x00};
      break;
    case CMD_READ_VOICE_SETTING:
      ret = {0x60, 0x00};
      break;
    case CMD_READ_NUM_BROADCAST:
      ret = {0x0A};
      break;
    case CMD_READ_DEFAULT_ERRONEOUS_REPORTING:
      ret = {0x00};
      break;
    case CMD_READ_FLOW_CONTROL_MODE:
      ret = {0x00};
      break;
    case CMD_READ_LE_HOST_SUPPORT:
      ret = {0x00, 0x01};  // LE 支持开
      break;
    case CMD_READ_PAGE_TIMEOUT:
      ret = {0x20, 0x00};
      break;
    case CMD_LE_SET_SCAN_PARAMETERS: {
      if (qn >= 2) le_scan_active_ = q[0] == 0x01;  // 01=active
      break;
    }

    // —— 以下命令统一回 CC SUCCESS ——
    case CMD_SET_EVENT_MASK:
    case CMD_SET_EVENT_FILTER:
    case CMD_WRITE_PAGE_TIMEOUT:
    case CMD_WRITE_AUTHENTICATION_ENABLE:
    case CMD_WRITE_CLASS_OF_DEVICE:
    case CMD_WRITE_INQUIRY_MODE:
    case CMD_WRITE_VOICE_SETTING:
    case CMD_HOST_BUFFER_SIZE:
    case CMD_HOST_NUM_COMPLETED_PACKETS:
    case CMD_WRITE_SIMPLE_PAIRING_MODE:
    case CMD_WRITE_SC_HOST_SUPPORT:
    case CMD_WRITE_LE_HOST_SUPPORT:
    case CMD_WRITE_PAGE_SCAN_ACTIVITY:
    case CMD_WRITE_INQUIRY_SCAN_ACTIVITY:
    case CMD_WRITE_PAGE_SCAN_TYPE:
    case CMD_WRITE_INQUIRY_SCAN_TYPE:
    case CMD_DELETE_STORED_LINK_KEY:
    case CMD_READ_STORED_LINK_KEY:
    case CMD_WRITE_PIN_TYPE:
    case CMD_WRITE_NUM_BROADCAST:
    case CMD_WRITE_DEFAULT_ERRONEOUS_REPORTING:
    case CMD_WRITE_FLOW_CONTROL_MODE:
    case CMD_LE_SET_EVENT_MASK:
    case CMD_LE_SET_RANDOM_ADDRESS:
    case CMD_LE_SET_ADVERTISING_PARAMETERS:
    case CMD_LE_SET_ADVERTISING_DATA:
    case CMD_LE_SET_SCAN_RESPONSE_DATA:
    case CMD_LE_SET_ADVERTISING_ENABLE:
    case CMD_LE_CLEAR_WHITE_LIST:
    case CMD_LE_ADD_DEVICE_TO_WHITE_LIST:
    case CMD_LE_REMOVE_DEVICE_FROM_WHITE_LIST:
    case CMD_LE_SET_HOST_CHANNEL_CLASSIFICATION:
      break;

    default:
      known = false;
      status = ST_UNKNOWN_CMD;
      ++unknown_count_;
      break;
  }

  // LE_Create_Connection 特例：Command Status(pending) + 由 tick/空口完成（M2）
  if (cmd.op == CMD_LE_CREATE_CONNECTION || cmd.op == CMD_LE_CREATE_CONNECTION_CANCEL ||
      cmd.op == CMD_CMD_DISCONNECT || cmd.op == CMD_LE_READ_REMOTE_FEATURES ||
      cmd.op == CMD_LE_READ_CHANNEL_MAP || cmd.op == CMD_LE_CONNECTION_UPDATE) {
    if (cmd.op == CMD_LE_CREATE_CONNECTION) {
      // params: scan_int(2) scan_win(2) filter(1) peer_type(1) peer_addr(6) ...
      if (qn >= 12) {
        std::lock_guard<std::mutex> lk(mx_);
        if (!conn_pending_ && conn_handle_ == 0) {
          conn_pending_ = true;
          conn_pending_type_ = q[5];
          conn_pending_peer_ = 0;
          for (int i = 0; i < 6; ++i) conn_pending_peer_ |= ((uint64_t)q[6 + i]) << (8 * i);
          conn_fire_at_ = kConnDelayTicks;
        }
      }
    } else if (cmd.op == CMD_LE_CREATE_CONNECTION_CANCEL) {
      std::lock_guard<std::mutex> lk(mx_);
      conn_pending_ = false;  // 幂等取消
    } else if (cmd.op == CMD_LE_READ_REMOTE_FEATURES && qn >= 2) {
      uint16_t h = (uint16_t)(q[0] | (q[1] << 8));
      std::lock_guard<std::mutex> lk(mx_);
      if (h == conn_handle_ && h != 0) {
        std::vector<uint8_t> rp{ST_SUCCESS, (uint8_t)(h & 0xFF), (uint8_t)(h >> 8),
                                0x01, 0x0F, 0, 0, 0, 0, 0, 0};
        delayed_events_.push_back({3, build_le_meta(LE_EVT_READ_REMOTE_FEATURES_COMPLETE, rp)});
      } else {
        // 未知句柄 → 延迟失败事件
        std::vector<uint8_t> rp{ST_UNKNOWN_CONN_ID, (uint8_t)(h & 0xFF), (uint8_t)(h >> 8),
                                0, 0, 0, 0, 0, 0, 0};
        delayed_events_.push_back({3, build_le_meta(LE_EVT_READ_REMOTE_FEATURES_COMPLETE, rp)});
      }
    }
    std::vector<uint8_t> evt = build_command_status(ST_SUCCESS, 1, cmd.op);
    {
      std::lock_guard<std::mutex> lk(mx_);
      event_q_.push_back(evt);
      ++event_count_;
      char buf[48];
      auto& names = command_names();
      auto it = names.find(cmd.op);
      std::snprintf(buf, sizeof(buf), "op=0x%04X(%s) st=CS", cmd.op, it != names.end() ? it->second : "?");
      op_log_.push_back(buf);
    }
    return;
  }

  std::vector<uint8_t> rp;
  rp.push_back(status);
  rp.insert(rp.end(), ret.begin(), ret.end());
  auto evt = build_command_complete(1, cmd.op, rp);
  {
    std::lock_guard<std::mutex> lk(mx_);
    event_q_.push_back(evt);
    ++event_count_;
    char buf[48];
    auto& names = command_names();
    auto it = names.find(cmd.op);
    std::snprintf(buf, sizeof(buf), "op=0x%04X(%s) st=%02X%s", cmd.op,
                  it != names.end() ? it->second : "?", status, known ? "" : " UNKNOWN");
    op_log_.push_back(buf);
  }
}

void MiniController::tick(uint64_t now_ms) {
  // 1) 空口：扫描中 → 广播报告事件
  if (le_scan_enabled_ || air_.scanning()) {
    air_.tick(now_ms, [this](std::vector<uint8_t> evt) { push_event(evt); });
  }
  // 2) 连接创建延迟
  std::lock_guard<std::mutex> lk(mx_);
  if (conn_pending_) {
    if (conn_fire_at_ > 0) {
      --conn_fire_at_;  // tick 计数（16ms 基 ×13 ≈ 208ms）—— 简化用计数器
    } else {
      conn_pending_ = false;
      auto dev = air_.find_connectable(conn_pending_peer_);
      uint8_t status = dev ? ST_SUCCESS : ST_UNKNOWN_CONN_ID;
      std::vector<uint8_t> p;
      p.push_back(status);
      p.push_back(next_handle_ & 0xFF);
      p.push_back(next_handle_ >> 8);
      p.push_back(0x00);  // role = central
      p.push_back(conn_pending_type_);
      for (int i = 0; i < 6; ++i) p.push_back((conn_pending_peer_ >> (8 * i)) & 0xFF);
      p.push_back(0x18); p.push_back(0x00);  // interval 30ms
      p.push_back(0x00); p.push_back(0x00);  // latency 0
      p.push_back(0x48); p.push_back(0x00);  // timeout 720ms
      p.push_back(0x0C);                     // master clock accuracy
      auto evt = build_le_meta(LE_EVT_CONNECTION_COMPLETE, p);
      event_q_.push_back(evt);
      ++event_count_;
      if (status == ST_SUCCESS) {
        conn_handle_ = next_handle_++;
        conn_peer_ = conn_pending_peer_;
        links_.alloc(conn_peer_, conn_pending_type_);
        // 延迟：LE Read Remote Features Complete（3 tick ≈ 48ms）
        std::vector<uint8_t> rp;
        rp.push_back(ST_SUCCESS);
        rp.push_back(conn_handle_ & 0xFF);
        rp.push_back(conn_handle_ >> 8);
        const uint8_t feat[8] = {0x01, 0x0F, 0, 0, 0, 0, 0, 0};
        rp.insert(rp.end(), feat, feat + 8);
        delayed_events_.push_back({3, build_le_meta(LE_EVT_READ_REMOTE_FEATURES_COMPLETE, rp)});
      }
    }
  }
  // 3) 延迟事件到期
  for (auto it = delayed_events_.begin(); it != delayed_events_.end();) {
    if (it->first == 0) {
      event_q_.push_back(it->second);
      ++event_count_;
      it = delayed_events_.erase(it);
    } else {
      --it->first;
      ++it;
    }
  }
}

void MiniController::simulate_disconnect(uint8_t reason) {
  std::lock_guard<std::mutex> lk(mx_);
  if (conn_handle_ == 0) return;
  uint16_t h = conn_handle_;
  conn_handle_ = 0;
  conn_peer_ = 0;
  links_.release(h);
  reasm_.reset();
  // Disconnection_Complete: status, handle(2), reason
  std::vector<uint8_t> p{ST_SUCCESS, (uint8_t)(h & 0xFF), (uint8_t)(h >> 8), reason};
  event_q_.push_back(build_event(EVT_DISCONNECTION_COMPLETE, p));
  ++event_count_;
}

}  // namespace pb::hci
