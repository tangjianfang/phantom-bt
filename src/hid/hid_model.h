#pragma once
// USB HID 设备模型：标准+HID 类控制请求、中断 IN/OUT 报告通道
#include <cstdint>
#include <memory>
#include <optional>
#include <deque>
#include <vector>

#include "hid/profiles.h"
#include "usb/control.h"
#include "usbip/protocol.h"

namespace pb::hid {

class HidModel : public pb::usb::IUsbDeviceModel {
 public:
  HidModel(const HidProfile& p, uint32_t busnum, uint32_t devnum);
  ~HidModel() override;

  const pb::usbip::ExportedDevice& exported() const override;
  void attach_sink(pb::usb::IUsbModelSink* sink) override;
  std::vector<uint8_t> on_control(const pb::usb::ControlSetup& s, const std::vector<uint8_t>& data) override;
  void on_out(uint8_t ep, std::vector<uint8_t> data) override;
  void on_in(uint8_t ep) override;
  void on_stall_clear(uint8_t ep) override;
  void on_detach() override;

  // 测试/控制 API
  void inject_input_report(std::vector<uint8_t> report);
  std::optional<std::vector<uint8_t>> pop_output_report();
  uint8_t idle() const { return idle_4ms_; }
  void set_idle(uint8_t d4) { idle_4ms_ = d4; }
  uint8_t protocol() const { return protocol_; }
  const std::vector<uint8_t>& report_descriptor() const;
  const HidProfile& profile() const { return profile_; }
  int dropped_reports() const { return dropped_; }
  bool stalled_recently = false;

 private:
  HidProfile profile_;
  pb::usbip::ExportedDevice exported_;
  pb::usb::DescriptorStore store_;
  std::vector<uint8_t> last_input_report_;
  pb::usb::IUsbModelSink* sink_ = nullptr;
  std::deque<std::vector<uint8_t>> input_queue_;
  std::deque<std::vector<uint8_t>> output_queue_;
  bool pending_in_ = false;
  uint8_t idle_4ms_ = 0;
  uint8_t protocol_ = 1;  // report protocol
  int dropped_ = 0;
  static constexpr size_t kQueueCap = 16;
};

}  // namespace pb::hid
