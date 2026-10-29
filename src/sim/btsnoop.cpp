#include "sim/btsnoop.h"

#include <cstring>

#include <windows.h>

#include "core/bytes.h"

namespace pb::sim {

namespace {
// btsnoop 时间纪元：0x00E03AB1A6764000 = 0000-01-01T00:00:00（微秒）
constexpr uint64_t kBtsnoopEpochUs = 0x00E03AB1A6764000ull;
uint64_t now_us_since_epoch() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;  // 1601 起 100ns
  return t / 10 + kBtsnoopEpochUs;  // 转微秒并平移到 btsnoop 纪元
}
}  // namespace

BtSnoopWriter::~BtSnoopWriter() { close(); }

bool BtSnoopWriter::open(const std::string& path) {
  std::lock_guard<std::mutex> lk(mx_);
  if (f_) return true;
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return false;
  ByteWriter w;
  const char ident[8] = {'b', 't', 's', 'n', 'o', 'o', 'p', 0};
  w.bytes((const uint8_t*)ident, 8);
  w.u32be(1);      // version
  w.u32be(1002);   // datalink: HCI UART (H4)
  auto h = w.take();
  std::fwrite(h.data(), 1, h.size(), f_);
  std::fflush(f_);
  records_ = 0;
  return true;
}

void BtSnoopWriter::close() {
  std::lock_guard<std::mutex> lk(mx_);
  if (f_) {
    std::fclose(f_);
    f_ = nullptr;
  }
}

void BtSnoopWriter::write_record(uint8_t h4_type, const uint8_t* data, size_t n) {
  std::lock_guard<std::mutex> lk(mx_);
  if (!f_) return;
  std::vector<uint8_t> pkt;
  pkt.reserve(n + 1);
  pkt.push_back(h4_type);
  pkt.insert(pkt.end(), data, data + n);

  ByteWriter w;
  w.u32be((uint32_t)pkt.size());  // orig_len
  w.u32be((uint32_t)pkt.size());  // incl_len
  w.u32be(0);                     // flags（主机→控制器方向可细分，此处统一 0）
  w.u32be(0);                     // cum_drops
  w.u64be(now_us_since_epoch());
  w.bytes(pkt.data(), pkt.size());
  auto rec = w.take();
  std::fwrite(rec.data(), 1, rec.size(), f_);
  ++records_;
}

void BtSnoopWriter::hci_command(const std::vector<uint8_t>& cmd) {
  write_record(0x01, cmd.data(), cmd.size());
}
void BtSnoopWriter::hci_event(const std::vector<uint8_t>& evt) {
  write_record(0x04, evt.data(), evt.size());
}
void BtSnoopWriter::acl_from_host(const hci::AclFrame& f) {
  auto b = hci::build_acl(f);
  write_record(0x02, b.data(), b.size());
}
void BtSnoopWriter::acl_to_host(const hci::AclFrame& f) {
  auto b = hci::build_acl(f);
  write_record(0x02, b.data(), b.size());
}

}  // namespace pb::sim
