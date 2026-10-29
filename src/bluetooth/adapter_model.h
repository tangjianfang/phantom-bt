#pragma once
// USB 蓝牙适配器模型（Bluetooth Core 5.4 Part B USB Transport Layer）
// if0(E0/01/01): EP0 类请求=HCI 命令 / 0x81 INT IN=HCI 事件 / 0x02 BULK OUT=ACL / 0x82 BULK IN=ACL
// if1(E0/01/01): 仅空 alt0（规范允许的零等时带宽合规形态，无 SCO）
#include <cstdint>
#include <memory>
#include <vector>

#include "hci/controller.h"
#include "usb/control.h"
#include "usbip/protocol.h"

namespace pb::bt {

class AdapterModel : public pb::usb::IUsbDeviceModel {
 public:
  AdapterModel(const pb::hci::ControllerConfig& cfg, uint32_t busnum, uint32_t devnum);
  ~AdapterModel() override;

  const pb::usbip::ExportedDevice& exported() const override;
  void attach_sink(pb::usb::IUsbModelSink* sink) override;
  std::vector<uint8_t> on_control(const pb::usb::ControlSetup& s, const std::vector<uint8_t>& data) override;
  void on_out(uint8_t ep, std::vector<uint8_t> data) override;
  void on_in(uint8_t ep) override;
  void on_detach() override;

  pb::hci::MiniController& controller() { return controller_; }

  // 观测
  int evt_tx_count() const { return evt_tx_; }
  int acl_tx_frames() const { return acl_tx_; }
  int acl_rx_frames() const { return acl_rx_; }

 private:
  void flush_event_if_pending();
  void flush_acl_if_pending();
  void send_event_now(std::vector<uint8_t> evt);
  void send_acl_now(pb::hci::AclFrame f);

  pb::hci::MiniController controller_;
  pb::usb::DescriptorStore store_;
  pb::usbip::ExportedDevice exported_;
  pb::usb::IUsbModelSink* sink_ = nullptr;
  bool pending_evt_ = false;
  bool pending_acl_ = false;
  std::vector<uint8_t> acl_reassembly_;
  int evt_tx_ = 0;
  int acl_tx_ = 0;
  int acl_rx_ = 0;
};

}  // namespace pb::bt
