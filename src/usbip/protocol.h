#pragma once
// USB/IP v1.1.1 线协议编解码（对端为 usbip-win2 客户端驱动）
// 全部多字节字段大端。依据：Linux 内核 drivers/usb/usbip/usbip_common.h
// usbip_usb_device 布局（312B）：path[256] busid[32] busnum(devnum,speed):u32×3
//   idVendor u16 idProduct u16 bcdDevice u16 + 6×u8(class,subclass,protocol,config,nconfigs,nifs)
#include <cstdint>
#include <string>
#include <vector>

namespace pb::usbip {

// 控制/枚举 opcodes
constexpr uint32_t OP_REQ_DEVLIST = 0x80000000u;
constexpr uint32_t OP_REP_DEVLIST = 0x00000001u;
constexpr uint32_t OP_REQ_IMPORT = 0x80000003u;
constexpr uint32_t OP_REP_IMPORT = 0x00000004u;
// URB opcodes
constexpr uint32_t USBIP_CMD_SUBMIT = 0x00000001u;
constexpr uint32_t USBIP_CMD_UNLINK = 0x00000002u;
constexpr uint32_t USBIP_RET_SUBMIT = 0x00000003u;
constexpr uint32_t USBIP_RET_UNLINK = 0x00000004u;

constexpr uint32_t DIR_OUT = 0;
constexpr uint32_t DIR_IN = 1;
constexpr uint32_t USBIP_SPEED_FULL = 2;  // 12Mbps

// URB 状态码（取自 linux/errno.h 通用值）
constexpr int32_t URB_STATUS_SUCCESS = 0;
constexpr int32_t URB_STATUS_EPIPE = -32;      // STALL
constexpr int32_t URB_STATUS_ECONNRESET = -104;
constexpr int32_t URB_STATUS_ENODEV = -19;

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
constexpr size_t RET_SUBMIT_HDR_SIZE = 40;
constexpr size_t RET_UNLINK_HDR_SIZE = 24;

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
