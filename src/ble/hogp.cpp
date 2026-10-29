#include "ble/hogp.h"

#include "ble/att.h"
#include "ble/services.h"

namespace pb::ble {

Hogp::Hogp(GattDb& db) : db_(db) {}

bool Hogp::ccc_subscribed(uint16_t ccc_handle) const {
  const GattAttr* a = db_.by_handle(ccc_handle);
  return a && a->value.size() >= 2 && (a->value[0] & 0x01);
}

void Hogp::on_write(uint16_t handle, const std::vector<uint8_t>& v) {
  if (handle == H_CTRL_POINT) {
    if (!v.empty()) suspended_ = v[0] == 0x00;  // 00=suspend 01=exit suspend
    return;
  }
  if (handle == H_PROTOCOL_MODE) {
    if (!v.empty()) protocol_mode_ = v[0];
    return;
  }
  if (handle == H_REPORT2_VALUE) {
    output_reports.push_back(v);
    return;
  }
}

bool Hogp::send_input_report(const std::vector<uint8_t>& keys8) {
  if (!ccc_subscribed(H_REPORT1_CCC)) {
    ++dropped_;
    return false;
  }
  db_.set_value(H_REPORT1_VALUE, keys8);
  if (notify_post) notify_post(AttServer::build_notification(H_REPORT1_VALUE, keys8));
  return true;
}

}  // namespace pb::ble
