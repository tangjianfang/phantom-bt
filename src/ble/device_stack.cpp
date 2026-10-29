#include "ble/device_stack.h"

#include "ble/services.h"

namespace pb::ble {

DeviceStack::DeviceStack(hci::MiniController& controller) : controller_(controller) {
  db_ = std::make_unique<GattDb>(GattDb::build(build_keyboard_attrs(ServiceSet{})));
  att_ = std::make_unique<AttServer>(*db_);
  hogp_ = std::make_unique<Hogp>(*db_);
  smp_ = std::make_unique<SmpResponder>();
  demux_ = std::make_unique<L2capFixedDemux>();
  rewire();
}

DeviceStack::~DeviceStack() = default;

void DeviceStack::rewire() {
  hogp_->notify_post = [this](const std::vector<uint8_t>& att_pdu) {
    controller_.send_l2cap(controller_.connection_handle(), build_l2cap(L2CAP_CID_ATT, att_pdu));
  };

  AttServer* att = att_.get();
  Hogp* hogp = hogp_.get();
  SmpResponder* smp = smp_.get();

  demux_->on_att = [att, hogp](const std::vector<uint8_t>& payload) {
    auto rsp = att->handle(payload.data(), payload.size());
    // 写完成钩子（CCC 订阅/协议模式/输出报告）
    if (payload.size() >= 3 && (payload[0] == ATT_WRITE_REQ || payload[0] == ATT_WRITE_CMD)) {
      uint16_t h = (uint16_t)(payload[1] | (payload[2] << 8));
      hogp->on_write(h, std::vector<uint8_t>(payload.begin() + 3, payload.end()));
    }
    return rsp ? *rsp : std::vector<uint8_t>{};
  };
  demux_->on_sm = [smp](const std::vector<uint8_t>& payload) {
    auto rsp = smp->handle(payload.data(), payload.size());
    return rsp ? *rsp : std::vector<uint8_t>{};
  };

  controller_.on_l2cap_from_host = [this](uint16_t, const std::vector<uint8_t>& pdu) {
    auto rsp = demux_->handle(pdu);
    if (!rsp.empty()) controller_.send_l2cap(controller_.connection_handle(), rsp);
  };
}

void DeviceStack::setup_keyboard(const std::string& device_name) {
  ServiceSet s;
  s.device_name = device_name;
  s.report_map = hid::template_ble_keyboard_reportmap();
  db_ = std::make_unique<GattDb>(GattDb::build(build_keyboard_attrs(s)));
  att_ = std::make_unique<AttServer>(*db_);
  hogp_ = std::make_unique<Hogp>(*db_);
  rewire();  // demux/notify 指向新对象
}

}  // namespace pb::ble
