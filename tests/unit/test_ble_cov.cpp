// 第 1 轮覆盖率补充（BLE 层未覆盖行）：
//  - att.cpp: ATT_READ_BY_TYPE 特征声明(0x2803)发现路径 / 未定义 opcode
//  - device_stack.cpp: send_input_report 线程安全入口
//  - ble_device.cpp: from_json 缺 kind / 缺 name 抛错分支
//  - smp.cpp: 未知 SMP 码静默分支
//  - gatt_db.cpp: find_value_by_type 未命中返回 nullptr
#include "test/test_framework.h"

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "ble/att.h"
#include "ble/ble_device.h"
#include "ble/device_stack.h"
#include "ble/gatt_db.h"
#include "ble/services.h"
#include "ble/smp.h"
#include "core/json.h"
#include "hci/controller.h"

using namespace pb;
using namespace pb::ble;

namespace {
GattDb keyboard_db() {
  ServiceSet s;  // 默认键盘属性表（句柄布局见 services.h）
  return GattDb::build(build_keyboard_attrs(s));
}
}  // namespace

// —— att.cpp 67-87：Read By Type → 特征声明发现 ——
PB_TEST(cov_ble_att_char_decl_discovery) {
  auto db = keyboard_db();
  AttServer att(db);
  // start=1 end=5 type=0x2803：decl 0x0002(DeviceName) 与 0x0004(Appearance)
  uint8_t req[] = {ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0x05, 0x00, 0x03, 0x28};
  auto rsp = att.handle(req, sizeof(req));
  PB_CHECK(rsp.has_value());
  // mtu=23 → fit=(23-2)/8=2 条；每条 handle(2)+props(1)+value_handle(2)+value_type(2)=7B
  PB_CHECK_EQ(rsp->size(), 2u + 2u * 7u);
  PB_CHECK_EQ((*rsp)[0], ATT_READ_BY_TYPE_RSP);
  PB_CHECK_EQ((*rsp)[1], 7u);
  // 第一条：decl 0x0002, props=P_READ(0x02), 值句柄 0x0003, 类型 0x2A00
  PB_CHECK_EQ((*rsp)[2], 0x02);
  PB_CHECK_EQ((*rsp)[3], 0x00);
  PB_CHECK_EQ((*rsp)[4], 0x02);
  PB_CHECK_EQ((*rsp)[5], 0x03);
  PB_CHECK_EQ((*rsp)[6], 0x00);
  PB_CHECK_EQ((*rsp)[7], 0x00);
  PB_CHECK_EQ((*rsp)[8], 0x2A);
  // 第二条：decl 0x0004 → 值句柄 0x0005, 类型 0x2A01
  PB_CHECK_EQ((*rsp)[9], 0x04);
  PB_CHECK_EQ((*rsp)[10], 0x00);
  PB_CHECK_EQ((*rsp)[11], 0x02);  // props
  PB_CHECK_EQ((*rsp)[12], 0x05);  // 值句柄 lo
  PB_CHECK_EQ((*rsp)[13], 0x00);  // 值句柄 hi
  PB_CHECK_EQ((*rsp)[14], 0x01);  // 0x2A01 LE lo
  PB_CHECK_EQ((*rsp)[15], 0x2A);  // 0x2A01 LE hi
}

PB_TEST(cov_ble_att_char_decl_mtu_truncation) {
  auto db = keyboard_db();
  AttServer att(db);
  db.mtu = 31;  // fit=(31-2)/8=3 → 1..0x000F 内 6 个 decl 只报前 3 个
  uint8_t req[] = {ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0x0F, 0x00, 0x03, 0x28};
  auto rsp = att.handle(req, sizeof(req));
  PB_CHECK(rsp.has_value());
  PB_CHECK_EQ(rsp->size(), 2u + 3u * 7u);
  PB_CHECK_EQ((*rsp)[2], 0x02);   // decl 0x0002
  PB_CHECK_EQ((*rsp)[9], 0x04);   // decl 0x0004
  PB_CHECK_EQ((*rsp)[16], 0x07);  // decl 0x0007（第 3 条后 fit=0 停止）
}

PB_TEST(cov_ble_att_char_decl_not_found) {
  auto db = keyboard_db();
  AttServer att(db);
  // 0x0011..0x0012 只有 ReportRef/CCC 描述符，无 char decl → Attribute Not Found
  uint8_t req[] = {ATT_READ_BY_TYPE_REQ, 0x11, 0x00, 0x12, 0x00, 0x03, 0x28};
  auto rsp = att.handle(req, sizeof(req));
  PB_CHECK(rsp.has_value());
  PB_CHECK_EQ(rsp->size(), 5u);
  PB_CHECK_EQ((*rsp)[0], ATT_ERROR);
  PB_CHECK_EQ((*rsp)[1], ATT_READ_BY_TYPE_REQ);
  PB_CHECK_EQ((*rsp)[2], 0x11);  // 出错句柄 = start
  PB_CHECK_EQ((*rsp)[3], 0x00);
  PB_CHECK_EQ((*rsp)[4], E_ATTR_NOT_FOUND);
}

PB_TEST(cov_ble_att_char_decl_missing_value_handle) {
  // decl 为库中最后一个属性 → by_handle(h+1) 为空时值句柄写 0
  GattAttr svc;
  svc.handle = 0x0001;
  svc.type = T_PRIMARY_SVC;
  svc.value = {0x00, 0x18};
  GattAttr decl;
  decl.handle = 0x0002;
  decl.type = T_CHAR_DECL;
  decl.props = P_READ;
  decl.value_type = U_DEVICE_NAME;
  auto db = GattDb::build({svc, decl});
  AttServer att(db);
  uint8_t req[] = {ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0x02, 0x00, 0x03, 0x28};
  auto rsp = att.handle(req, sizeof(req));
  PB_CHECK(rsp.has_value());
  PB_CHECK_EQ(rsp->size(), 9u);  // op + len + 单条 7B
  PB_CHECK_EQ((*rsp)[0], ATT_READ_BY_TYPE_RSP);
  PB_CHECK_EQ((*rsp)[1], 7u);
  PB_CHECK_EQ((*rsp)[2], 0x02);  // decl 句柄 0x0002
  PB_CHECK_EQ((*rsp)[3], 0x00);
  PB_CHECK_EQ((*rsp)[4], 0x02);  // props
  PB_CHECK_EQ((*rsp)[5], 0x00);  // 值句柄缺失 → 0
  PB_CHECK_EQ((*rsp)[6], 0x00);
  PB_CHECK_EQ((*rsp)[7], 0x00);  // value_type 0x2A00 LE
  PB_CHECK_EQ((*rsp)[8], 0x2A);
}

// —— att.cpp 160：未定义 opcode → Invalid PDU ——
PB_TEST(cov_ble_att_unknown_opcode_invalid_pdu) {
  auto db = keyboard_db();
  AttServer att(db);
  uint8_t pdu[] = {0x77};  // 任意未收录 opcode
  auto rsp = att.handle(pdu, sizeof(pdu));
  PB_CHECK(rsp.has_value());
  PB_CHECK_EQ(rsp->size(), 5u);
  PB_CHECK_EQ((*rsp)[0], ATT_ERROR);
  PB_CHECK_EQ((*rsp)[1], 0x77);
  PB_CHECK_EQ((*rsp)[2], 0x00);  // handle 0
  PB_CHECK_EQ((*rsp)[3], 0x00);
  PB_CHECK_EQ((*rsp)[4], E_INVALID_PDU);
}

// —— device_stack.cpp 54-57：send_input_report（线程安全入口）——
PB_TEST(cov_ble_device_stack_send_input_report) {
  hci::MiniController ctl(hci::ControllerConfig{});
  DeviceStack stack(ctl);
  const std::vector<uint8_t> keys = {0x02, 0x00, 'x', 0, 0, 0, 0, 0};
  // CCC 未订阅 → 拒发并计 dropped
  PB_CHECK(!stack.send_input_report(keys));
  PB_CHECK_EQ(stack.hogp().dropped_reports(), 1);
  // 订阅 Report CCC 后成功，且报告值写入 GATT
  PB_CHECK_EQ(stack.db().write_value(H_REPORT1_CCC, {0x01, 0x00}), 0);
  PB_CHECK(stack.send_input_report(keys));
  auto [data, err] = stack.db().read_value(H_REPORT1_VALUE, 0);
  PB_CHECK_EQ(err, 0);
  PB_CHECK_EQ(data.size(), 8u);
  PB_CHECK_EQ(data[0], 0x02);
  PB_CHECK_EQ(data[2], 'x');
}

// —— ble_device.cpp 40/44：from_json 缺 kind / 缺 name ——
PB_TEST(cov_ble_profile_missing_kind_and_name_throw) {
  bool threw = false;
  try {
    BleDeviceDef::from_json(json::parse(R"({"name":"n","address":"C0:00:00:00:00:01"})"));
  } catch (const std::exception& e) {
    threw = true;
    PB_CHECK(std::string(e.what()).find("kind") != std::string::npos);
  }
  PB_CHECK(threw);

  threw = false;
  try {
    BleDeviceDef::from_json(
        json::parse(R"({"kind":"keyboard","address":"C0:00:00:00:00:01"})"));
  } catch (const std::exception& e) {
    threw = true;
    PB_CHECK(std::string(e.what()).find("name") != std::string::npos);
  }
  PB_CHECK(threw);
}

// —— smp.cpp 79：密钥分发等未处理码静默 ——
PB_TEST(cov_ble_smp_unhandled_code_silent) {
  SmpResponder smp;
  // 0x07 = Encryption Information（密钥分发阶段）：应答端无响应、不影响配对状态
  const uint8_t pdu[] = {0x07, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  auto rsp = smp.handle(pdu, sizeof(pdu));
  PB_CHECK(!rsp.has_value());
  PB_CHECK(!smp.paired());
}

// —— gatt_db.cpp 50：find_value_by_type 未命中 ——
PB_TEST(cov_ble_gatt_find_value_by_type_miss) {
  auto db = keyboard_db();
  PB_CHECK(db.find_value_by_type(U_BATTERY_LEVEL) != nullptr);  // 命中（对照）
  PB_CHECK(db.find_value_by_type(0x2A58) == nullptr);           // 类型不存在
  // after 越过唯一命中句柄（0x0018）→ 无更靠后的属性
  PB_CHECK(db.find_value_by_type(U_BATTERY_LEVEL, H_BATTERY_VALUE) == nullptr);
  // 经 ATT：值类型不存在 → Attribute Not Found
  AttServer att(db);
  uint8_t req[] = {ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0xFF, 0xFF, 0x58, 0x2A};
  auto rsp = att.handle(req, sizeof(req));
  PB_CHECK(rsp.has_value());
  PB_CHECK_EQ((*rsp)[0], ATT_ERROR);
  PB_CHECK_EQ((*rsp)[4], E_ATTR_NOT_FOUND);
}

// ===================== 缺陷修复锚定（R4）：bug id "hogp-suspend"（评审 #37） ==============
// （PB_TEST 名即 C 标识符，bug id 中的 '-' 映射为 '_'）
// 缺陷：Hogp::on_write 对 Control Point(H_CTRL_POINT) 写 0x00 只置 suspended_
// 标志，而 send_input_report 从不检查它 —— suspend 状态机是死代码，主机挂起设备
// 后输入通知照发（src/ble/hogp.cpp 原实现）。
// 正确语义（HOGP profile 1.0 §3.3.3 Suspend/Exit Suspend）：suspend(0x00) 期间
// 暂停输入报告通知（计 dropped），exit-suspend(0x01) 恢复。
PB_TEST(bug_hogp_suspend_pauses_input_notifications) {
  auto db = keyboard_db();
  Hogp hogp(db);
  std::vector<std::vector<uint8_t>> posted;
  hogp.notify_post = [&](const std::vector<uint8_t>& p) { posted.push_back(p); };
  db.write_value(H_REPORT1_CCC, {0x01, 0x00});  // 先订阅：排除 CCC 未订阅导致的 drop
  PB_CHECK(hogp.send_input_report({0x02, 0, 'h', 0, 0, 0, 0, 0}));
  PB_CHECK_EQ(posted.size(), 1u);

  hogp.on_write(H_CTRL_POINT, {0x00});  // suspend
  PB_CHECK(hogp.suspended());
  PB_CHECK(!hogp.send_input_report({0x02, 0, 'h', 0, 0, 0, 0, 0}));  // 挂起：必须拒绝
  PB_CHECK(!hogp.send_input_report({0x00, 0, 0, 0, 0, 0, 0, 0}));    // 空抬起帧同样拒绝
  PB_CHECK_EQ(posted.size(), 1u);                                    // 零通知
  PB_CHECK_EQ(hogp.dropped_reports(), 2);                            // 挂起丢弃计入观测

  hogp.on_write(H_CTRL_POINT, {0x01});  // exit suspend
  PB_CHECK(!hogp.suspended());
  PB_CHECK(hogp.send_input_report({0x02, 0, 'h', 0, 0, 0, 0, 0}));  // 恢复通知
  PB_CHECK_EQ(posted.size(), 2u);
}
