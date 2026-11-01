#pragma once
// BLE 设备栈组装：MiniController + L2CAP/ATT/GATT/HOGP/SMP
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

  // 注意：这些访问器供单线程测试/观测使用；跨线程数据面走锁保护的入口
  Hogp& hogp() { return *hogp_; }
  GattDb& db() { return *db_; }
  SmpResponder& smp() { return *smp_; }
  hci::MiniController& controller() { return controller_; }

  // 线程安全入口：usbip 连接线程（ATT 写 CCC）与 GUI/CLI/pipe 线程（注入通知）
  // 并发触碰 GattDb/Hogp；setup_keyboard 还会销毁重建对象 —— 统一用 mx_ 串行化
  bool send_input_report(const std::vector<uint8_t>& keys8);

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
  std::mutex mx_;  // 保护 db_/att_/hogp_/smp_/demux_ 及其内部容器
};

}  // namespace pb::ble
