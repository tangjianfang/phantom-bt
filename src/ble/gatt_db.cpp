#include "ble/gatt_db.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pb::ble {

GattDb GattDb::build(std::vector<GattAttr> attrs) {
  GattDb db;
  uint16_t prev = 0;
  for (auto& a : attrs) {
    if (a.handle == 0 || a.handle <= prev) {
      throw std::runtime_error("gatt db: handles must be increasing (got " +
                               std::to_string(a.handle) + " after " + std::to_string(prev) + ")");
    }
    prev = a.handle;
    db.index_[a.handle] = db.attrs_.size();
    db.attrs_.push_back(std::move(a));
  }
  return db;
}

const GattAttr* GattDb::by_handle(uint16_t h) const {
  auto it = index_.find(h);
  return it == index_.end() ? nullptr : &attrs_[it->second];
}

std::vector<std::pair<uint16_t, uint16_t>> GattDb::service_ranges() const {
  std::vector<std::pair<uint16_t, uint16_t>> out;
  for (size_t i = 0; i < attrs_.size(); ++i) {
    if (attrs_[i].type != T_PRIMARY_SVC) continue;
    uint16_t start = attrs_[i].handle;
    uint16_t end = 0xFFFF;
    for (size_t j = i + 1; j < attrs_.size(); ++j) {
      if (attrs_[j].type == T_PRIMARY_SVC) {
        end = attrs_[j].handle - 1;
        break;
      }
    }
    if (end == 0xFFFF) end = attrs_.back().handle;
    out.emplace_back(start, end);
  }
  return out;
}

const GattAttr* GattDb::find_value_by_type(uint16_t type, uint16_t after) const {
  for (auto& a : attrs_)
    if (a.type == type && a.handle > after) return &a;
  return nullptr;
}

std::pair<std::vector<uint8_t>, uint8_t> GattDb::read_value(uint16_t h, uint16_t offset) const {
  const GattAttr* a = by_handle(h);
  if (!a || a->type == T_PRIMARY_SVC || a->type == T_CHAR_DECL) return {{}, E_INVALID_HANDLE};
  if (!a->readable) return {{}, E_READ_NOT_PERMITTED};
  if (offset > a->value.size()) return {{}, E_INVALID_OFFSET};
  return {std::vector<uint8_t>(a->value.begin() + offset, a->value.end()), 0};
}

uint8_t GattDb::write_value(uint16_t h, const std::vector<uint8_t>& v) {
  GattAttr* a = nullptr;
  {
    auto it = index_.find(h);
    if (it == index_.end()) return E_INVALID_HANDLE;
    a = &attrs_[it->second];
  }
  if (a->type == T_PRIMARY_SVC || a->type == T_CHAR_DECL) return E_WRITE_NOT_PERMITTED;
  if (!a->writable) return E_WRITE_NOT_PERMITTED;
  a->value = v;
  return 0;
}

void GattDb::set_value(uint16_t h, const std::vector<uint8_t>& v) {
  auto it = index_.find(h);
  if (it != index_.end()) attrs_[it->second].value = v;
}

std::vector<std::pair<uint16_t, uint16_t>> GattDb::descriptors_in_range(uint16_t start,
                                                                        uint16_t end) const {
  std::vector<std::pair<uint16_t, uint16_t>> out;
  for (auto& a : attrs_) {
    if (a.handle < start || a.handle > end) continue;
    if (a.type == T_CCC || a.type == T_REPORT_REF) out.emplace_back(a.handle, a.type);
  }
  return out;
}

}  // namespace pb::ble
