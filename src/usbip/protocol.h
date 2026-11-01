#pragma once
// USB/IP v1.1.1 线协议编解码（对端为 usbip-win2 客户端驱动）
// 全部多字节字段大端。依据：Linux 内核 drivers/usb/usbip/usbip_common.h
// usbip_usb_device 布局（312B）：path[256] busid[32] busnum(devnum,speed):u32×3
//   idVendor u16 idProduct u16 bcdDevice u16 + 6×u8(class,subclass,protocol,config,nconfigs,nifs)
#include <cstdint>
#include <string>
#include <vector>

namespace pb::usbip {

// op 阶段公共头 op_common（Linux usbip_common.h / usbip-win proto_op.h）：
//   u16 version(BE)=0x0111 + u16 code(BE) + u32 status(BE)，请求/应答同为 8B。
// 控制/枚举 opcodes（code 字段取值，与 usbip.exe 客户端一致）
constexpr uint16_t OP_VERSION = 0x0111;
constexpr uint16_t OP_REQ_DEVLIST = 0x8005;
constexpr uint16_t OP_REP_DEVLIST = 0x0005;
constexpr uint16_t OP_REQ_IMPORT = 0x8003;
constexpr uint16_t OP_REP_IMPORT = 0x0003;
// URB opcodes
constexpr uint32_t USBIP_CMD_SUBMIT = 0x00000001u;
constexpr uint32_t USBIP_CMD_UNLINK = 0x00000002u;
constexpr uint32_t USBIP_RET_SUBMIT = 0x00000003u;
constexpr uint32_t USBIP_RET_UNLINK = 0x00000004u;

constexpr uint32_t DIR_OUT = 0;
constexpr uint32_t DIR_IN = 1;
constexpr uint32_t USBIP_SPEED_FULL = 2;  // 12Mbps
constexpr uint32_t USBIP_SPEED_HIGH = 3;  // 480Mbps（usbip-win2 UDE 根集线器按 HS 规则校验端口，
                                          // bulk 512/bcdUSB 2.00 形态须配 speed=3 宣告）

// URB 状态码（取自 linux/errno.h 通用值）
constexpr int32_t URB_STATUS_SUCCESS = 0;
constexpr int32_t URB_STATUS_EPIPE = -32;      // STALL
constexpr int32_t URB_STATUS_ECONNRESET = -104;
constexpr int32_t URB_STATUS_ENODEV = -19;
constexpr int32_t URB_STATUS_EINVAL = -22;     // 参数非法（如超限 transfer_buffer_length）

// CMD_SUBMIT transfer_buffer_length 钳位上限（16MB）：tbl 由对端完全控制，
// 超限值不得进入 data.resize/recv 路径（单包巨额分配可打挂进程），须回 EINVAL
constexpr int32_t MAX_TRANSFER_BUFFER = 16 * 1024 * 1024;

struct ExportedInterface {
  uint8_t iclass, isubclass, iprotocol;
};

struct ExportedDevice {
  std::string path;    // 如 "/phantom/7-1"
  std::string busid;   // 如 "7-1"
  uint32_t busnum = 7, devnum = 1, speed = USBIP_SPEED_FULL;
  uint16_t vid = 0, pid = 0, bcd_device = 0x0100;
  uint8_t dev_class = 0, dev_subclass = 0, dev_protocol = 0;
  uint8_t config_value = 1, num_configs = 1, num_interfaces = 1;
  std::vector<ExportedInterface> interfaces;
};

struct SubmitHeader {
  uint32_t command, seqnum, devid, direction, ep;
  uint32_t transfer_flags;
  int32_t transfer_buffer_length;
  int32_t start_frame;
  uint32_t number_of_packets;
  int32_t interval;
  uint8_t setup[8];
};

constexpr size_t USB_DEVICE_PACKED_SIZE = 312;  // usbip_usb_device
constexpr size_t IFACE_PACKED_SIZE = 4;         // usbip_usb_interface
constexpr size_t CMD_SUBMIT_HDR_SIZE = 48;
constexpr size_t CMD_UNLINK_HDR_SIZE = 24;
// RET_SUBMIT/RET_UNLINK 与 CMD_* 同为 usbip_header union 定长 48B
//（basic20 + RET_SUBMIT 尾部 20B + setup[8] 占位 = 48；usbip-win
// static_assert(sizeof(header)==48)，客户端按 PDU 固定读 48B）
constexpr size_t RET_SUBMIT_HDR_SIZE = 48;
constexpr size_t RET_UNLINK_HDR_SIZE = 48;

// 打包
std::vector<uint8_t> pack_op_header(uint32_t op, int32_t status);
std::vector<uint8_t> pack_usb_device(const ExportedDevice& d);   // 312B（devlist 与 rep_import 共用）
std::vector<uint8_t> pack_rep_devlist(const std::vector<ExportedDevice>& devs);
std::vector<uint8_t> pack_rep_import(const ExportedDevice& d);
std::vector<uint8_t> pack_ret_submit(uint32_t seqnum, uint32_t devid, uint32_t ep, uint32_t direction,
                                     int32_t status, int32_t actual_length, const uint8_t* data,
                                     size_t data_len);
std::vector<uint8_t> pack_ret_unlink(uint32_t seqnum, uint32_t devid, uint32_t ep, int32_t status);

// 解析
bool parse_submit(const uint8_t* p, size_t n, SubmitHeader& out);
bool parse_unlink(const uint8_t* p, size_t n, uint32_t& seqnum, uint32_t& devid, uint32_t& ep,
                  uint32_t& unlink_seqnum);
// 从 OP_REQ_IMPORT 请求体（4B opcode 之后）提取 busid
bool parse_import_busid(const uint8_t* p, size_t n, std::string& busid);

}  // namespace pb::usbip
