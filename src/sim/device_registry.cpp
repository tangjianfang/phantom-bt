#include "sim/device_registry.h"

namespace pb::sim {

std::string DeviceRegistry::add(std::shared_ptr<pb::usb::IUsbDeviceModel> dev) {
  std::lock_guard<std::mutex> lk(mx_);
  // 优先使用设备自带 busid（模型 exported() 与导入协议一致）；空则自动分配
  std::string busid = dev->exported().busid;
  if (busid.empty()) busid = "7-" + std::to_string(next_);
  ++next_;
  devices_.emplace_back(busid, std::move(dev));
  return busid;
}

bool DeviceRegistry::remove(const std::string& busid) {
  std::lock_guard<std::mutex> lk(mx_);
  for (auto it = devices_.begin(); it != devices_.end(); ++it) {
    if (it->first == busid) {
      devices_.erase(it);
      return true;
    }
  }
  return false;
}

std::shared_ptr<pb::usb::IUsbDeviceModel> DeviceRegistry::find(const std::string& busid) const {
  std::lock_guard<std::mutex> lk(mx_);
  for (auto& [id, dev] : devices_)
    if (id == busid) return dev;
  return nullptr;
}

std::vector<pb::usbip::ExportedDevice> DeviceRegistry::list() const {
  std::lock_guard<std::mutex> lk(mx_);
  std::vector<pb::usbip::ExportedDevice> out;
  out.reserve(devices_.size());
  for (auto& [id, dev] : devices_) out.push_back(dev->exported());
  return out;
}

size_t DeviceRegistry::size() const {
  std::lock_guard<std::mutex> lk(mx_);
  return devices_.size();
}

}  // namespace pb::sim
