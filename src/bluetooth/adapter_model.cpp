#include "bluetooth/adapter_model.h"

#include <cstdio>

#include "usb/descriptors.h"

namespace pb::bt {

AdapterModel::AdapterModel(const pb::hci::ControllerConfig& cfg, uint32_t busnum, uint32_t devnum)
    : controller_(cfg) {
  pb::usb::ConfigParams c;
  c.vid = 0x0B00;  // 测试用非常见 VID：避免 Windows 触发厂商初始化路径（R-111 缓解）
  c.pid = 0x0001;
  c.product = "Phantom-BT Radio";
  c.manufacturer = "Phantom-BT";
  c.serial = "PB-RADIO-0001";
  c.max_packet0 = 64;
  c.device_class = 0xE0;
  c.device_subclass = 0x01;
  c.device_protocol = 0x01;
  c.bcd_usb = 0x0110;  // FS 蓝牙惯例
  c.self_powered = false;
  c.remote_wakeup = false;
  pb::usb::InterfaceDef i0;
  i0.number = 0;
  i0.iclass = 0xE0;
  i0.isubclass = 0x01;
  i0.iprotocol = 0x01;
  i0.endpoints = {
      {0x81, 3, 16, 1},  // HCI 事件：INT IN，max 16，1ms
      {0x02, 2, 64, 0},  // ACL OUT
      {0x82, 2, 64, 0},  // ACL IN
  };
  pb::usb::InterfaceDef i1;  // SCO：仅空 alt0（无端点）
  i1.number = 1;
  i1.iclass = 0xE0;
  i1.isubclass = 0x01;
  i1.iprotocol = 0x01;
  c.interfaces = {i0, i1};

  store_.device = pb::usb::build_device_descriptor(c);
  store_.config = pb::usb::build_config_descriptor(c);
  store_.strings[0] = pb::usb::build_string_descriptor_zero();
  store_.strings[1] = pb::usb::build_string_descriptor(c.manufacturer);
  store_.strings[2] = pb::usb::build_string_descriptor(c.product);
  store_.strings[3] = pb::usb::build_string_descriptor(c.serial);

  exported_.path = "/phantom/" + std::to_string(busnum) + "-" + std::to_string(devnum);
  exported_.busid = std::to_string(busnum) + "-" + std::to_string(devnum);
  exported_.busnum = busnum;
  exported_.devnum = devnum;
  exported_.vid = c.vid;
  exported_.pid = c.pid;
  exported_.dev_class = 0xE0;
  exported_.dev_subclass = 0x01;
  exported_.dev_protocol = 0x01;
  exported_.num_interfaces = 2;
  exported_.interfaces.push_back({0xE0, 0x01, 0x01});
  exported_.interfaces.push_back({0xE0, 0x01, 0x01});
}

AdapterModel::~AdapterModel() = default;

const pb::usbip::ExportedDevice& AdapterModel::exported() const { return exported_; }
void AdapterModel::attach_sink(pb::usb::IUsbModelSink* s) { sink_ = s; }

std::vector<uint8_t> AdapterModel::on_control(const pb::usb::ControlSetup& s,
                                              const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  if (pb::usb::descriptor_request(s, store_, out)) return out;

  uint8_t type = s.bmRequestType & 0x60;

  // HCI 命令类请求：bmRequestType=0x20（当前规范）/ 0xE0（历史值），bRequest=0x00
  if (type == 0x20 && s.bmRequestType == 0x20 && s.bRequest == 0x00) {
    controller_.handle_command(data.data(), data.size());
    flush_event_if_pending();
    return {};
  }
  if (s.bmRequestType == 0xE0 && s.bRequest == 0x00) {  // 历史形态兼容
    controller_.handle_command(data.data(), data.size());
    flush_event_if_pending();
    return {};
  }

  throw pb::usb::ControlStall("bt: unsupported control request");
}

void AdapterModel::on_out(uint8_t ep, std::vector<uint8_t> data) {
  if (ep != 0x02) return;
  // BULK OUT ACL 流：按帧头 length 重组完整 ACL 帧
  acl_reassembly_.insert(acl_reassembly_.end(), data.begin(), data.end());
  while (true) {
    if (acl_reassembly_.size() < 4) return;
    uint16_t len = (uint16_t)(acl_reassembly_[2] | (acl_reassembly_[3] << 8));
    if (acl_reassembly_.size() < 4u + len) return;
    pb::hci::AclFrame f;
    pb::hci::parse_acl(acl_reassembly_.data(), 4 + len, f);
    ++acl_rx_;
    controller_.handle_acl_from_host(f);
    acl_reassembly_.erase(acl_reassembly_.begin(), acl_reassembly_.begin() + 4 + len);
    flush_acl_if_pending();
  }
}

void AdapterModel::on_in(uint8_t ep) {
  std::vector<uint8_t> evt;
  if (ep == 0x81) {
    if (controller_.pop_event(evt)) {
      send_event_now(std::move(evt));
    } else {
      pending_evt_ = true;
    }
    return;
  }
  if (ep == 0x82) {
    pb::hci::AclFrame f;
    if (controller_.pop_acl_to_host(f)) {
      send_acl_now(std::move(f));
    } else {
      pending_acl_ = true;
    }
    return;
  }
}

void AdapterModel::send_event_now(std::vector<uint8_t> evt) {
  ++evt_tx_;
  if (sink_) sink_->complete_in(0x81, 0, std::move(evt));
}

void AdapterModel::send_acl_now(pb::hci::AclFrame f) {
  // 控制器→主机方向按 64B 切包（BULK max 64；M3 起按 le_acl_mtu-4 生成完整帧后由 USB 分包）
  auto bytes = pb::hci::build_acl(f);
  ++acl_tx_;
  if (sink_) sink_->complete_in(0x82, 0, std::move(bytes));
}

void AdapterModel::flush_event_if_pending() {
  if (!pending_evt_) return;
  std::vector<uint8_t> evt;
  if (controller_.pop_event(evt)) {
    pending_evt_ = false;
    send_event_now(std::move(evt));
  }
}

void AdapterModel::flush_acl_if_pending() {
  if (!pending_acl_) return;
  pb::hci::AclFrame f;
  if (controller_.pop_acl_to_host(f)) {
    pending_acl_ = false;
    send_acl_now(std::move(f));
  }
}

void AdapterModel::on_detach() {
  pending_evt_ = false;
  pending_acl_ = false;
  acl_reassembly_.clear();
}

}  // namespace pb::bt
