#include "ble/ble_device.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace pb::ble {

uint64_t BleDeviceDef::parse_address(std::string_view s) {
  uint64_t v = 0;
  int shift = 0;
  size_t i = s.size();
  int seen = 0;
  while (i > 0 && seen < 6) {
    size_t start = i;
    while (start > 0 && s[start - 1] != ':') --start;
    std::string_view byte = s.substr(start, i - start);
    if (byte.empty()) throw std::runtime_error("ble address: bad format '" + std::string(s) + "'");
    v |= (uint64_t)std::stoul(std::string(byte), nullptr, 16) << shift;
    shift += 8;
    ++seen;
    i = start > 0 ? start - 1 : 0;
  }
  if (seen != 6) throw std::runtime_error("ble address: need 6 octets '" + std::string(s) + "'");
  return v;
}

std::string BleDeviceDef::format_address(uint64_t addr) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                (unsigned)((addr >> 40) & 0xFF), (unsigned)((addr >> 32) & 0xFF),
                (unsigned)((addr >> 24) & 0xFF), (unsigned)((addr >> 16) & 0xFF),
                (unsigned)((addr >> 8) & 0xFF), (unsigned)(addr & 0xFF));
  return buf;
}

BleDeviceDef BleDeviceDef::from_json(const pb::json::Value& v) {
  BleDeviceDef d;
  if (auto* f = v.find("kind")) d.kind = f->as_str();
  else throw std::runtime_error("ble profile: missing 'kind'");
  if (d.kind != "keyboard" && d.kind != "beacon")
    throw std::runtime_error("ble profile: unknown kind '" + d.kind + "'");
  if (auto* f = v.find("name")) d.name = f->as_str();
  else throw std::runtime_error("ble profile: missing 'name'");
  if (auto* f = v.find("address")) d.address = parse_address(f->as_str());
  else throw std::runtime_error("ble profile: missing 'address'");
  if (auto* f = v.find("connectable")) d.connectable = f->as_bool();
  if (auto* f = v.find("hid_map")) d.hid_map = f->as_str();
  return d;
}

BleDeviceDef BleDeviceDef::load_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("ble profile: cannot open '" + path + "'");
  std::stringstream ss;
  ss << in.rdbuf();
  return from_json(pb::json::parse(ss.str()));
}

}  // namespace pb::ble
