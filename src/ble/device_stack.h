#pragma once
// BLE 设备栈组装：MiniController + L2CAP/ATT/GATT/HOGP/SMP
#include <memory>

#include "ble/att.h"
#include "ble/gatt_db.h"
#include "ble/hogp.h"
#include "ble/l2cap.h"
#include "ble/smp.h"
#include "hci/controller.h"
#include "hid/report_descriptor.h"

namespace pb::ble {

class DeviceStack {
 public:
  explicit DeviceStack(hci::MiniController& controller);
  ~DeviceStack();

  Hogp& hogp() { return *hogp_; }
  GattDb& db() { return *db_; }
  SmpResponder& smp() { return *smp_; }
  hci::MiniController& controller() { return controller_; }

  // 用键盘 Profile 装配（GAP+HID+Battery，句柄布局见 services.h）
  void setup_keyboard(const std::string& device_name);

 private:
  void rewire();  // demux/notify 回调绑定当前 db/att/hogp/smp 对象
  hci::MiniController& controller_;
  std::unique_ptr<GattDb> db_;
  std::unique_ptr<AttServer> att_;
  std::unique_ptr<Hogp> hogp_;
  std::unique_ptr<SmpResponder> smp_;
  std::unique_ptr<L2capFixedDemux> demux_;
};

}  // namespace pb::ble
