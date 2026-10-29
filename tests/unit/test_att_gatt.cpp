#include "test/test_framework.h"
#include "ble/att.h"
#include "ble/gatt_db.h"
#include "ble/hogp.h"
#include "ble/l2cap.h"
#include "ble/services.h"
#include "hid/report_descriptor.h"

using namespace pb;
using namespace pb::ble;

namespace {

GattDb make_db() {
  ServiceSet s;
  s.device_name = "PhantomKb";
  s.report_map = hid::template_ble_keyboard_reportmap();
  return GattDb::build(build_keyboard_attrs(s));
}

}  // namespace

// —— GattDb ——
PB_TEST(gatt_db_handle_validation) {
  auto attrs = build_keyboard_attrs(ServiceSet{});
  PB_CHECK_THROWS(GattDb::build({attrs[0], attrs[0]}));  // 重复句柄
  PB_CHECK_THROWS(GattDb::build({GattAttr{.handle = 5}, GattAttr{.handle = 3}}));
  auto db = GattDb::build(attrs);
  PB_CHECK(db.by_handle(0x0001) != nullptr);
  PB_CHECK(db.by_handle(0x9999) == nullptr);
}

PB_TEST(gatt_service_ranges) {
  auto db = make_db();
  auto ranges = db.service_ranges();
  PB_CHECK_EQ(ranges.size(), 3u);
  PB_CHECK_EQ(ranges[0].first, 0x0001u);
  PB_CHECK_EQ(ranges[0].second, 0x0005u);  // GAP → HID-1
  PB_CHECK_EQ(ranges[1].first, 0x0006u);
  PB_CHECK_EQ(ranges[1].second, 0x0015u);  // HID → Battery-1
  PB_CHECK_EQ(ranges[2].first, 0x0016u);
  PB_CHECK_EQ(ranges[2].second, 0x0019u);  // 末服务到最后一属性
}

PB_TEST(gatt_read_write_semantics) {
  auto db = make_db();
  auto [ok, err] = db.read_value(H_DEVICE_NAME, 0);
  PB_CHECK_EQ(err, 0);
  PB_CHECK_EQ(std::string(ok.begin(), ok.end()), std::string("PhantomKb"));
  auto [off, e2] = db.read_value(H_DEVICE_NAME, 10);  // > 名长 9 → 非法
  PB_CHECK_EQ(e2, E_INVALID_OFFSET);
  auto [_, e3] = db.read_value(0x0900, 0);
  PB_CHECK_EQ(e3, E_INVALID_HANDLE);
  PB_CHECK_EQ(db.write_value(H_DEVICE_NAME, {1}), E_WRITE_NOT_PERMITTED);
  PB_CHECK_EQ(db.write_value(H_REPORT1_CCC, {0x01, 0x00}), 0);  // CCC 可写
}

// —— ATT ——
PB_TEST(att_mtu_exchange) {
  auto db = make_db();
  AttServer att(db);
  uint8_t mtuq[] = {ATT_MTU_REQ, 0xF4, 0x00};
  auto rsp = att.handle(mtuq, 3);
  PB_CHECK_EQ((*rsp)[0], ATT_MTU_RSP);
  PB_CHECK_EQ((*rsp)[1], 0xF7);        // 应答为服务器 MTU 247
  PB_CHECK_EQ((*rsp)[2], 0x00);
  PB_CHECK_EQ(db.mtu, 244u);           // 有效 MTU = min(247, 244)
  PB_CHECK_EQ(att.peer_mtu(), 244u);
}

PB_TEST(att_read_by_group_primary_services) {
  auto db = make_db();
  AttServer att(db);
  // req: start=1 end=0xFFFF type=0x2800
  uint8_t req[] = {ATT_READ_BY_GROUP_REQ, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28};
  auto rsp = att.handle(req, 7);
  PB_CHECK_EQ((*rsp)[0], ATT_READ_BY_GROUP_RSP);
  PB_CHECK_EQ((*rsp)[1], 6u);
  // 第一条：GAP 01 00 05 00 00 18
  PB_CHECK_EQ((*rsp)[2], 0x01);
  PB_CHECK_EQ((*rsp)[3], 0x00);
  PB_CHECK_EQ((*rsp)[4], 0x05);
  PB_CHECK_EQ((*rsp)[5], 0x00);
  PB_CHECK_EQ((*rsp)[6], 0x00);
  PB_CHECK_EQ((*rsp)[7], 0x18);
  // 第二条：HID 06 00 15 00 12 18
  PB_CHECK_EQ((*rsp)[8], 0x06);
  PB_CHECK_EQ((*rsp)[12], 0x12);
  PB_CHECK_EQ((*rsp)[13], 0x18);
  // 3 服务 × 6B + 2 头 = 20
  PB_CHECK_EQ(rsp->size(), 20u);
}

PB_TEST(att_read_by_type_hid_info) {
  auto db = make_db();
  AttServer att(db);
  uint8_t req[] = {ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0xFF, 0xFF, 0x4A, 0x2A};
  auto rsp = att.handle(req, 7);
  PB_CHECK_EQ((*rsp)[0], ATT_READ_BY_TYPE_RSP);
  PB_CHECK_EQ((*rsp)[1], 6u);           // handle(2) + HID Info 值(4)
  PB_CHECK_EQ((*rsp)[2], 0x08);         // HID Info 值句柄 0x0008
  PB_CHECK_EQ((*rsp)[4], 0x01);         // bcdHID lo
  PB_CHECK_EQ((*rsp)[6], 0x00);         // country
  PB_CHECK_EQ((*rsp)[7], 0x02);         // flags
}

PB_TEST(att_read_report_map_and_blob) {
  auto db = make_db();
  AttServer att(db);
  db.mtu = 23;
  uint8_t req[] = {ATT_READ_REQ, 0x0A, 0x00};
  auto rsp = att.handle(req, 3);
  PB_CHECK_EQ((*rsp)[0], ATT_READ_RSP);
  PB_CHECK_EQ(rsp->size(), 1u + 22u);  // mtu-1 截断

  // blob 偏移 22 → 剩余
  uint8_t blob[] = {ATT_READ_BLOB_REQ, 0x0A, 0x00, 22, 0x00};
  auto rsp2 = att.handle(blob, 5);
  PB_CHECK_EQ((*rsp2)[0], ATT_READ_BLOB_RSP);
  auto full_map = hid::template_ble_keyboard_reportmap();
  size_t rest = full_map.size() - 22;
  PB_CHECK_EQ(rsp2->size(), 1u + std::min<size_t>(rest, 22));
}

PB_TEST(att_error_unknown_handle) {
  auto db = make_db();
  AttServer att(db);
  uint8_t req[] = {ATT_READ_REQ, 0x99, 0x00};
  auto rsp = att.handle(req, 3);
  PB_CHECK_EQ((*rsp)[0], ATT_ERROR);
  PB_CHECK_EQ((*rsp)[1], ATT_READ_REQ);
  PB_CHECK_EQ((*rsp)[2], 0x99);
  PB_CHECK_EQ((*rsp)[4], E_INVALID_HANDLE);
}

PB_TEST(att_find_info_descriptors) {
  auto db = make_db();
  AttServer att(db);
  // 范围 0x0011..0x0019：ReportRef 11、CCC 12、ReportRef 15、CCC 19
  uint8_t req[] = {ATT_FIND_INFO_REQ, 0x11, 0x00, 0x19, 0x00};
  auto rsp = att.handle(req, 5);
  PB_CHECK_EQ((*rsp)[0], ATT_FIND_INFO_RSP);
  PB_CHECK_EQ((*rsp)[1], 0x01);  // 16-bit 格式
  // 4 对 = 16B + 2 = 18
  PB_CHECK_EQ(rsp->size(), 18u);
  PB_CHECK_EQ((*rsp)[2], 0x11);
  PB_CHECK_EQ((*rsp)[4], 0x08);
  PB_CHECK_EQ((*rsp)[5], 0x29);  // 0x2908
}

PB_TEST(att_write_req_and_cmd) {
  auto db = make_db();
  AttServer att(db);
  // Write Request CCC=0100
  uint8_t wr[] = {ATT_WRITE_REQ, 0x12, 0x00, 0x01, 0x00};
  auto rsp = att.handle(wr, 5);
  PB_CHECK_EQ((*rsp)[0], ATT_WRITE_RSP);
  auto* a = db.by_handle(H_REPORT1_CCC);
  PB_CHECK_EQ(a->value[0], 0x01);
  // Write Command（0x52）无应答
  uint8_t wc[] = {ATT_WRITE_CMD, 0x12, 0x00, 0x00, 0x00};
  auto rsp2 = att.handle(wc, 5);
  PB_CHECK(!rsp2.has_value());
  PB_CHECK_EQ(db.by_handle(H_REPORT1_CCC)->value[0], 0x00);
}

PB_TEST(att_unsupported_pdu) {
  auto db = make_db();
  AttServer att(db);
  uint8_t prep[] = {ATT_PREPARE_WRITE_REQ, 0};
  auto rsp = att.handle(prep, 2);
  PB_CHECK_EQ((*rsp)[0], ATT_ERROR);
  PB_CHECK_EQ((*rsp)[4], E_UNSUPPORTED_REQ);
}

// —— HOGP ——
PB_TEST(hogp_requires_ccc_before_notify) {
  auto db = make_db();
  Hogp hogp(db);
  std::vector<std::vector<uint8_t>> posted;
  hogp.notify_post = [&](const std::vector<uint8_t>& p) { posted.push_back(p); };
  PB_CHECK(!hogp.send_input_report({0x02, 0, 'h', 0, 0, 0, 0, 0}));
  PB_CHECK_EQ(hogp.dropped_reports(), 1);
  PB_CHECK(posted.empty());
  db.write_value(H_REPORT1_CCC, {0x01, 0x00});
  PB_CHECK(hogp.send_input_report({0x02, 0, 'h', 0, 0, 0, 0, 0}));
  PB_CHECK_EQ(posted.size(), 1u);
  // ATT 通知：1B 0x1B + handle 10 00 + 8B
  PB_CHECK_EQ(posted[0][0], ATT_HANDLE_VALUE_NTF);
  PB_CHECK_EQ(posted[0][1], 0x10);
  PB_CHECK_EQ(posted[0][3], 0x02);  // mod
  PB_CHECK_EQ(posted[0][4], 0x00);
  PB_CHECK_EQ(posted[0][5], 'h');
}

PB_TEST(hogp_protocol_mode_and_control_point) {
  auto db = make_db();
  Hogp hogp(db);
  PB_CHECK_EQ(hogp.protocol_mode(), 0x01);
  hogp.on_write(H_PROTOCOL_MODE, {0x00});  // boot
  PB_CHECK_EQ(hogp.protocol_mode(), 0x00);
  hogp.on_write(H_CTRL_POINT, {0x00});  // suspend
  PB_CHECK(hogp.suspended());
  hogp.on_write(H_CTRL_POINT, {0x01});
  PB_CHECK(!hogp.suspended());
}

PB_TEST(hogp_output_report_capture) {
  auto db = make_db();
  Hogp hogp(db);
  hogp.on_write(H_REPORT2_VALUE, {0xAA});
  hogp.on_write(H_REPORT2_VALUE, {0xBB, 0xCC});
  PB_CHECK_EQ(hogp.output_reports.size(), 2u);
  PB_CHECK_EQ(hogp.output_reports[1][1], 0xCC);
}

// —— L2CAP ——
PB_TEST(l2cap_build_and_parse) {
  auto pdu = build_l2cap(0x0004, {0x01, 0x02});
  PB_CHECK(pdu == std::vector<uint8_t>({0x02, 0x00, 0x04, 0x00, 0x01, 0x02}));
  uint16_t cid;
  std::vector<uint8_t> payload;
  PB_CHECK(parse_l2cap(pdu, cid, payload));
  PB_CHECK_EQ(cid, 4u);
  PB_CHECK_EQ(payload.size(), 2u);
  PB_CHECK(!parse_l2cap({0x05, 0x00, 0x04, 0x00, 0x01}, cid, payload));  // len 撒谎
}

PB_TEST(l2cap_demux_routes_att) {
  L2capFixedDemux demux;
  bool att_called = false, sm_called = false;
  demux.on_att = [&](const std::vector<uint8_t>& p) {
    att_called = true;
    PB_CHECK_EQ(p[0], ATT_READ_REQ);
    return std::vector<uint8_t>{ATT_READ_RSP, 0x41};
  };
  demux.on_sm = [&](const std::vector<uint8_t>&) {
    sm_called = true;
    return std::vector<uint8_t>{};
  };
  auto rsp = demux.handle(build_l2cap(0x0004, {ATT_READ_REQ, 0x03, 0x00}));
  PB_CHECK(att_called);
  PB_CHECK(!sm_called);
  // 应答被 L2CAP 包裹：02 00 04 00 02 41
  PB_CHECK(rsp == std::vector<uint8_t>({0x02, 0x00, 0x04, 0x00, 0x13, 0x41}));  // len2 + cid4 + {13,41}

  demux.handle(build_l2cap(0x0006, {0x01}));
  PB_CHECK(sm_called);
  // 未知 CID 忽略
  auto r3 = demux.handle(build_l2cap(0x0041, {0x01}));
  PB_CHECK(r3.empty());
}
