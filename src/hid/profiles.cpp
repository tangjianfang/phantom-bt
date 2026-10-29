#include "hid/profiles.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/bytes.h"

namespace pb::hid {

uint16_t parse_u16(std::string_view s) {
  if (s.empty()) throw std::runtime_error("parse_u16: empty");
  int base = 10;
  if (s.size() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
    s.remove_prefix(2);
    base = 16;
  }
  uint32_t v = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v > 0xFFFF) {
    throw std::runtime_error("parse_u16: invalid value '" + std::string(s) + "'");
  }
  return (uint16_t)v;
}

HidProfile HidProfile::from_json(const pb::json::Value& v) {
  HidProfile p;
  if (auto* t = v.find("template")) {
    p.template_name = t->as_str();
  } else {
    throw std::runtime_error("profile: missing 'template'");
  }
  if (p.template_name != "gamepad" && p.template_name != "keyboard" && p.template_name != "mouse" &&
      p.template_name != "vendor") {
    throw std::runtime_error("profile: unknown template '" + p.template_name + "'");
  }
  if (auto* f = v.find("vid")) p.vid = parse_u16(f->as_str());
  else throw std::runtime_error("profile: missing 'vid'");
  if (auto* f = v.find("pid")) p.pid = parse_u16(f->as_str());
  else throw std::runtime_error("profile: missing 'pid'");
  if (auto* f = v.find("product")) p.product = f->as_str();
  if (auto* f = v.find("manufacturer")) p.manufacturer = f->as_str();
  if (auto* f = v.find("serial")) p.serial = f->as_str();
  if (auto* f = v.find("report_descriptor_hex")) {
    p.report_descriptor_override = from_hex(f->as_str());
  }
  return p;
}

HidProfile HidProfile::load_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("profile: cannot open '" + path + "'");
  std::stringstream ss;
  ss << in.rdbuf();
  auto v = pb::json::parse(ss.str());
  return from_json(v);
}

}  // namespace pb::hid
