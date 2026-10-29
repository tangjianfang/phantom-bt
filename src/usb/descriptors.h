#pragma once
// USB 描述符构建（多字节字段小端，USB 规范）
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pb::usb {

struct EndpointDef {
  uint8_t address;      // 如 0x81 INT IN
  uint8_t attributes;   // 3=interrupt, 2=bulk
  uint16_t max_packet;
  uint8_t interval;     // interrupt: ms; bulk: 0
};

struct HidClassDescriptor {  // 9B 类描述符（bDescriptorType=0x21）
  uint16_t bcdHid = 0x0111;
  uint8_t country = 0;
  uint8_t num_descriptors = 1;
  uint8_t report_type = 0x22;
  uint16_t report_length = 0;
};

struct InterfaceDef {
  uint8_t number;
  uint8_t alt_setting;
  uint8_t iclass;
  uint8_t isubclass;
  uint8_t iprotocol;
  uint8_t str_index = 0;
  std::optional<HidClassDescriptor> hid;
  std::vector<EndpointDef> endpoints;
};

struct IadDef {  // 8B（bDescriptorType=0x0B）
  uint8_t first_interface;
  uint8_t interface_count;
  uint8_t iclass;
  uint8_t isubclass;
  uint8_t iprotocol;
  uint8_t str_index = 0;
};

struct ConfigParams {
  uint16_t vid = 0, pid = 0;
  std::string product = "Phantom-BT Device";
  std::string manufacturer = "Phantom-BT";
  std::string serial = "000000000001";
  uint8_t max_packet0 = 8;
  uint8_t device_class = 0, device_subclass = 0, device_protocol = 0;  // 设备描述符级
  uint16_t bcd_usb = 0x0200;
  uint16_t bcd_device = 0x0100;
  bool self_powered = false;
  bool remote_wakeup = true;
  std::vector<IadDef> iads;
  std::vector<InterfaceDef> interfaces;
};

std::vector<uint8_t> build_device_descriptor(const ConfigParams& c);       // 18B
std::vector<uint8_t> build_config_descriptor(const ConfigParams& c);
std::vector<uint8_t> build_string_descriptor_zero();                       // LANGID → 4B
std::vector<uint8_t> build_string_descriptor(std::string_view utf8);       // UTF-8 → UTF-16LE

}  // namespace pb::usb
