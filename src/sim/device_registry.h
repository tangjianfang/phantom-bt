#pragma once
// 设备注册表：busid → 设备模型
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "usb/control.h"
#include "usbip/protocol.h"

namespace pb::sim {

class DeviceRegistry {
 public:
  // 返回分配的 busid（"7-N"，N 从 1 递增）
  std::string add(std::shared_ptr<pb::usb::IUsbDeviceModel> dev);
  bool remove(const std::string& busid);
  std::shared_ptr<pb::usb::IUsbDeviceModel> find(const std::string& busid) const;
  std::vector<pb::usbip::ExportedDevice> list() const;
  size_t size() const;

 private:
  mutable std::mutex mx_;
  std::vector<std::pair<std::string, std::shared_ptr<pb::usb::IUsbDeviceModel>>> devices_;
  uint32_t next_ = 1;
};

}  // namespace pb::sim
