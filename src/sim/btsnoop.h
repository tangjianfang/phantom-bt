#pragma once
// btsnoop v1 日志（Wireshark 兼容，datalink 1002 = HCI UART H4）
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "hci/packets.h"

namespace pb::sim {

class BtSnoopWriter {
 public:
  ~BtSnoopWriter();
  bool open(const std::string& path);  // 头: "btsnoop\0" + BE u32 1 + BE u32 1002
  void close();
  void hci_command(const std::vector<uint8_t>& cmd);   // H4 0x01
  void hci_event(const std::vector<uint8_t>& evt);     // H4 0x04
  void acl_from_host(const hci::AclFrame& f);          // H4 0x02
  void acl_to_host(const hci::AclFrame& f);            // H4 0x02
  uint64_t record_count() const { return records_; }
  bool is_open() const { return f_ != nullptr; }

 private:
  void write_record(uint8_t h4_type, const uint8_t* data, size_t n);
  std::mutex mx_;
  FILE* f_ = nullptr;
  uint64_t records_ = 0;
};

}  // namespace pb::sim
