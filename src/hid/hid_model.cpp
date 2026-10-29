#include "hid/hid_model.h"

#include <cstring>

#include "hid/report_descriptor.h"
#include "usb/descriptors.h"

namespace pb::hid {

HidModel::HidModel(const HidProfile& p, uint32_t busnum, uint32_t devnum) : profile_(p) {
  std::vector<uint8_t> rdesc;
  if (p.report_descriptor_override) {
    rdesc = *p.report_descriptor_override;
  } else if (p.template_name == "keyboard") {
    rdesc = template_keyboard();
  } else if (p.template_name == "mouse") {
    rdesc = template_mouse();
  } else if (p.template_name == "gamepad") {
    rdesc = template_gamepad();
  } else {
    rdesc = template_vendor();
  }

  // USB 形态：单接口 HID，INT IN 0x81 + INT OUT 0x01
  pb::usb::ConfigParams c;
  c.vid = p.vid;
  c.pid = p.pid;
  c.product = p.product;
  c.manufacturer = p.manufacturer;
  c.serial = p.serial;
  c.max_packet0 = 64;
  pb::usb::InterfaceDef intf;
  intf.number = 0;
  intf.iclass = 3;
  intf.isubclass = 0;
  intf.iprotocol = p.template_name == "keyboard" ? 1 : p.template_name == "mouse" ? 2 : 0;
  intf.hid = pb::usb::HidClassDescriptor{.report_length = (uint16_t)rdesc.size()};
  intf.endpoints = {{0x81, 3, 64, 1}, {0x01, 3, 64, 1}};
  c.interfaces = {intf};

  store_.device = pb::usb::build_device_descriptor(c);
  store_.config = pb::usb::build_config_descriptor(c);
  store_.strings[0] = pb::usb::build_string_descriptor_zero();
  store_.strings[1] = pb::usb::build_string_descriptor(p.manufacturer);
  store_.strings[2] = pb::usb::build_string_descriptor(p.product);
  store_.strings[3] = pb::usb::build_string_descriptor(p.serial);
  store_.report_descriptor = std::move(rdesc);

  exported_.path = "/phantom/" + std::to_string(busnum) + "-" + std::to_string(devnum);
  exported_.busid = std::to_string(busnum) + "-" + std::to_string(devnum);
  exported_.busnum = busnum;
  exported_.devnum = devnum;
  exported_.vid = p.vid;
  exported_.pid = p.pid;
  exported_.dev_class = 0;
  exported_.num_interfaces = 1;
  exported_.interfaces.push_back({3, 0, intf.iprotocol});
}

HidModel::~HidModel() = default;

const pb::usbip::ExportedDevice& HidModel::exported() const { return exported_; }
void HidModel::attach_sink(pb::usb::IUsbModelSink* s) { sink_ = s; }

const std::vector<uint8_t>& HidModel::report_descriptor() const { return store_.report_descriptor; }

std::vector<uint8_t> HidModel::on_control(const pb::usb::ControlSetup& s,
                                          const std::vector<uint8_t>& data) {
  // 标准请求先走通用分发
  std::vector<uint8_t> out;
  if (pb::usb::descriptor_request(s, store_, out)) return out;

  stalled_recently = false;
  uint8_t type = s.bmRequestType & 0x60;
  uint8_t recipient = s.bmRequestType & 0x1F;
  bool direction_in = s.bmRequestType & 0x80;

  if (type == 0x00 && recipient == 0x01 && s.bRequest == pb::usb::REQ_GET_DESCRIPTOR &&
      (s.wValue >> 8) == 0x22) {
    // HID 报告描述符（标准类型、接口接收者）
    out = store_.report_descriptor;
    if (out.size() > s.wLength) out.resize(s.wLength);
    return out;
  }

  if (type == 0x20) {  // 类请求（接口接收者）
    switch (s.bRequest) {
      case 0x01: {  // GET_REPORT
        if (!direction_in) break;
        std::vector<uint8_t> r = last_input_report_;
        if (r.empty()) r.assign(store_.report_descriptor.empty() ? 8 : 8, 0);
        if (r.size() > s.wLength) r.resize(s.wLength);
        return r;
      }
      case 0x02:  // GET_IDLE
        if (!direction_in) break;
        return {idle_4ms_};
      case 0x03:  // GET_PROTOCOL（仅 boot 设备有意义，统一支持）
        if (!direction_in) break;
        return {protocol_};
      case 0x09:  // SET_REPORT（输出报告经控制端点）
        output_queue_.push_back(data);
        if (output_queue_.size() > kQueueCap) output_queue_.pop_front();
        return {};
      case 0x0A:  // SET_IDLE
        idle_4ms_ = s.wValue >> 8;
        return {};
      case 0x0B:  // SET_PROTOCOL
        protocol_ = s.wValue & 0xFF;
        return {};
      default:
        break;
    }
  }

  stalled_recently = true;
  throw pb::usb::ControlStall("hid: unsupported control request");
}

void HidModel::on_out(uint8_t ep, std::vector<uint8_t> data) {
  if (ep == 0x01) {  // INT OUT → 输出报告
    output_queue_.push_back(std::move(data));
    if (output_queue_.size() > kQueueCap) output_queue_.pop_front();
  }
}

void HidModel::on_in(uint8_t ep) {
  if (ep != 0x81) return;
  if (!input_queue_.empty()) {
    auto r = std::move(input_queue_.front());
    input_queue_.pop_front();
    last_input_report_ = r;
    if (sink_) sink_->complete_in(0x81, 0, std::move(r));
  } else {
    pending_in_ = true;
  }
}

void HidModel::inject_input_report(std::vector<uint8_t> report) {
  if (pending_in_ && sink_) {
    pending_in_ = false;
    last_input_report_ = report;
    sink_->complete_in(0x81, 0, std::move(report));
    return;
  }
  if (input_queue_.size() >= kQueueCap) {
    input_queue_.pop_front();
    ++dropped_;
  }
  input_queue_.push_back(std::move(report));
}

std::optional<std::vector<uint8_t>> HidModel::pop_output_report() {
  if (output_queue_.empty()) return std::nullopt;
  auto r = std::move(output_queue_.front());
  output_queue_.pop_front();
  return r;
}

void HidModel::on_stall_clear(uint8_t) { stalled_recently = false; }
void HidModel::on_detach() { pending_in_ = false; }

}  // namespace pb::hid
