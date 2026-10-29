#include "usb/descriptors.h"

#include <cstring>

#include "core/bytes.h"

namespace pb::usb {

namespace {
constexpr uint8_t DT_DEVICE = 1, DT_CONFIG = 2, DT_STRING = 3, DT_INTERFACE = 4, DT_ENDPOINT = 5,
                  DT_IAD = 0x0B, DT_HID = 0x21;

uint8_t attributes_byte(const ConfigParams& c) {
  uint8_t a = 0x80;
  if (c.self_powered) a |= 0x40;
  if (c.remote_wakeup) a |= 0x20;
  return a;
}
}  // namespace

std::vector<uint8_t> build_device_descriptor(const ConfigParams& c) {
  ByteWriter w;
  w.u8(18);
  w.u8(DT_DEVICE);
  w.u16le(c.bcd_usb);
  w.u8(c.device_class);
  w.u8(c.device_subclass);
  w.u8(c.device_protocol);
  w.u8(c.max_packet0);
  w.u16le(c.vid);
  w.u16le(c.pid);
  w.u16le(c.bcd_device);
  w.u8(1);  // iManufacturer
  w.u8(2);  // iProduct
  w.u8(3);  // iSerialNumber
  w.u8(1);  // bNumConfigurations
  return w.take();
}

std::vector<uint8_t> build_config_descriptor(const ConfigParams& c) {
  // 先组装接口块以计算总长
  std::vector<uint8_t> ifaces;
  {
    ByteWriter w;
    for (const auto& i : c.interfaces) {
      // IAD（若该接口是某 IAD 的首接口）
      for (const auto& iad : c.iads) {
        if (iad.first_interface == i.number) {
          w.u8(8);
          w.u8(DT_IAD);
          w.u8(iad.first_interface);
          w.u8(iad.interface_count);
          w.u8(iad.iclass);
          w.u8(iad.isubclass);
          w.u8(iad.iprotocol);
          w.u8(iad.str_index);
        }
      }
      w.u8(9);
      w.u8(DT_INTERFACE);
      w.u8(i.number);
      w.u8(i.alt_setting);
      w.u8((uint8_t)i.endpoints.size());
      w.u8(i.iclass);
      w.u8(i.isubclass);
      w.u8(i.iprotocol);
      w.u8(i.str_index);
      if (i.hid) {
        w.u8(9);
        w.u8(DT_HID);
        w.u16le(i.hid->bcdHid);
        w.u8(i.hid->country);
        w.u8(i.hid->num_descriptors);
        w.u8(i.hid->report_type);
        w.u16le(i.hid->report_length);
      }
      for (const auto& e : i.endpoints) {
        w.u8(7);
        w.u8(DT_ENDPOINT);
        w.u8(e.address);
        w.u8(e.attributes);
        w.u16le(e.max_packet);
        w.u8(e.interval);
      }
    }
    ifaces = w.take();
  }

  ByteWriter w;
  uint16_t total = (uint16_t)(9 + ifaces.size());
  w.u8(9);
  w.u8(DT_CONFIG);
  w.u16le(total);
  w.u8((uint8_t)c.interfaces.size());
  w.u8(1);  // bConfigurationValue
  w.u8(0);  // iConfiguration
  w.u8(attributes_byte(c));
  w.u8(50);  // bMaxPower = 50×2mA = 100mA
  w.bytes(ifaces.data(), ifaces.size());
  return w.take();
}

std::vector<uint8_t> build_string_descriptor_zero() {
  ByteWriter w;
  w.u8(4);
  w.u8(DT_STRING);
  w.u16le(0x0409);  // English (United States)
  return w.take();
}

std::vector<uint8_t> build_string_descriptor(std::string_view utf8) {
  // UTF-8 → UTF-16LE（BMP 内字符逐一转换；ASCII 快速路径）
  ByteWriter w;
  std::vector<uint8_t> body;
  size_t i = 0;
  while (i < utf8.size()) {
    unsigned cp = (unsigned char)utf8[i];
    size_t adv = 1;
    if (cp >= 0xF0 && i + 3 < utf8.size()) {
      cp = ((cp & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) | ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F);
      adv = 4;
    } else if (cp >= 0xE0 && i + 2 < utf8.size()) {
      cp = ((cp & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
      adv = 3;
    } else if (cp >= 0xC0 && i + 1 < utf8.size()) {
      cp = ((cp & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
      adv = 2;
    }
    // 代理对拆为两个 UTF-16 单元
    if (cp >= 0x10000) {
      unsigned v = cp - 0x10000;
      body.push_back((v >> 10) & 0xFF);
      body.push_back(0xD8 | (((v >> 10) >> 8) & 0x03));
      body.push_back(v & 0xFF);
      body.push_back(0xDC | (((v & 0x3FF) >> 8) & 0x03));
    } else {
      body.push_back(cp & 0xFF);
      body.push_back((cp >> 8) & 0xFF);
    }
    i += adv;
  }
  size_t len = body.size() + 2;
  if (len > 255) {  // 描述符长度上限
    body.resize(253);
    len = 255;
  }
  w.u8((uint8_t)len);
  w.u8(DT_STRING);
  w.bytes(body.data(), body.size());
  return w.take();
}

}  // namespace pb::usb
