#include "usbip/protocol.h"

#include <cstring>

#include "core/bytes.h"

namespace pb::usbip {

std::vector<uint8_t> pack_op_header(uint32_t op, int32_t status) {
  ByteWriter w;
  w.u32be(op);
  w.u32be((uint32_t)status);
  return w.take();
}

std::vector<uint8_t> pack_usb_device(const ExportedDevice& d) {
  ByteWriter w;
  // path[256]
  char path[256] = {};
  std::memcpy(path, d.path.data(), std::min<size_t>(d.path.size(), 255));
  w.bytes((const uint8_t*)path, 256);
  // busid[32]
  char busid[32] = {};
  std::memcpy(busid, d.busid.data(), std::min<size_t>(d.busid.size(), 31));
  w.bytes((const uint8_t*)busid, 32);
  w.u32be(d.busnum);
  w.u32be(d.devnum);
  w.u32be(d.speed);
  w.u16be(d.vid);
  w.u16be(d.pid);
  w.u16be(d.bcd_device);
  w.u8(d.dev_class);
  w.u8(d.dev_subclass);
  w.u8(d.dev_protocol);
  w.u8(d.config_value);
  w.u8(d.num_configs);
  w.u8(d.num_interfaces);
  return w.take();
}

std::vector<uint8_t> pack_rep_devlist(const std::vector<ExportedDevice>& devs) {
  ByteWriter w;
  w.u32be(OP_REP_DEVLIST);
  w.u32be(0);  // status
  w.u32be((uint32_t)devs.size());
  for (const auto& d : devs) {
    auto dev = pack_usb_device(d);
    w.bytes(dev.data(), dev.size());
    for (const auto& i : d.interfaces) {
      w.u8(i.iclass);
      w.u8(i.isubclass);
      w.u8(i.iprotocol);
      w.u8(0);  // padding
    }
  }
  return w.take();
}

std::vector<uint8_t> pack_rep_import(const ExportedDevice& d) {
  ByteWriter w;
  w.u32be(OP_REP_IMPORT);
  w.u32be(0);  // status = 0 成功
  auto dev = pack_usb_device(d);
  w.bytes(dev.data(), dev.size());
  return w.take();
}

std::vector<uint8_t> pack_ret_submit(uint32_t seqnum, uint32_t devid, uint32_t ep, uint32_t direction,
                                     int32_t status, int32_t actual_length, const uint8_t* data,
                                     size_t data_len) {
  ByteWriter w;
  w.u32be(USBIP_RET_SUBMIT);
  w.u32be(seqnum);
  w.u32be(devid);
  w.u32be(direction);
  w.u32be(ep);
  w.u32be(status);
  w.u32be((uint32_t)actual_length);
  w.u32be(0);  // start_frame
  w.u32be(0);  // number_of_packets（不支持 iso）
  w.u32be(0);  // error_count
  if (direction == DIR_IN && data && data_len) w.bytes(data, data_len);
  return w.take();
}

std::vector<uint8_t> pack_ret_unlink(uint32_t seqnum, uint32_t devid, uint32_t ep, int32_t status) {
  ByteWriter w;
  w.u32be(USBIP_RET_UNLINK);
  w.u32be(seqnum);
  w.u32be(devid);
  w.u32be(DIR_OUT);
  w.u32be(ep);
  w.u32be((uint32_t)status);
  return w.take();
}

bool parse_submit(const uint8_t* p, size_t n, SubmitHeader& out) {
  if (n < CMD_SUBMIT_HDR_SIZE) return false;
  ByteReader r(p, n);
  out.command = r.u32be();
  out.seqnum = r.u32be();
  out.devid = r.u32be();
  out.direction = r.u32be();
  out.ep = r.u32be();
  out.transfer_flags = r.u32be();
  out.transfer_buffer_length = (int32_t)r.u32be();
  out.start_frame = (int32_t)r.u32be();
  out.number_of_packets = r.u32be();
  out.interval = (int32_t)r.u32be();
  for (int i = 0; i < 8; ++i) out.setup[i] = r.u8();
  return r.ok() && out.command == USBIP_CMD_SUBMIT;
}

bool parse_unlink(const uint8_t* p, size_t n, uint32_t& seqnum, uint32_t& devid, uint32_t& ep,
                  uint32_t& unlink_seqnum) {
  if (n < CMD_UNLINK_HDR_SIZE) return false;
  ByteReader r(p, n);
  uint32_t command = r.u32be();
  seqnum = r.u32be();
  devid = r.u32be();
  uint32_t direction = r.u32be();
  ep = r.u32be();
  unlink_seqnum = r.u32be();
  return r.ok() && command == USBIP_CMD_UNLINK && direction == DIR_OUT;
}

bool parse_import_busid(const uint8_t* p, size_t n, std::string& busid) {
  if (n < 32) return false;
  busid.assign((const char*)p, strnlen((const char*)p, 32));
  return !busid.empty();
}

}  // namespace pb::usbip
