#pragma once
// USB/IP TCP 服务器：devlist / import / URB 循环
#include <memory>
#include <string>
#include <vector>

#include "sim/device_registry.h"

namespace pb::usbip {

class Server {
 public:
  Server(pb::sim::DeviceRegistry& reg, uint16_t port);
  ~Server();
  bool start(std::string* err);
  void stop();
  uint16_t port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pb::usbip
