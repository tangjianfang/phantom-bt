// Linux 宿主单测链接用：usbip 传输、Winsock 与物理电台不在本机实现。
#include "core/bt_radio.h"
#include "core/net.h"
#include "usbip/server.h"

namespace pb::usbip {

struct Server::Impl {};

Server::Server(pb::sim::DeviceRegistry&, uint16_t) {}
Server::~Server() = default;
bool Server::start(std::string* err) {
  if (err) *err = "usbip server is not built on this host";
  return false;
}
void Server::stop() {}
uint16_t Server::port() const { return 0; }

}  // namespace pb::usbip

namespace pb::net {

WsaSession::WsaSession() {}
WsaSession::~WsaSession() {}

}  // namespace pb::net

namespace pb::btradio {

bool is_our_virtual(const std::string&) { return false; }
bool is_physical_radio_id(const std::string& instance_id) {
  return instance_id.rfind("USB\\", 0) == 0;
}
std::vector<RadioDevice> list_radios() { return {}; }
int set_radios_enabled(bool, std::string* err) {
  if (err) *err = "physical radio control requires Windows";
  return 0;
}

}  // namespace pb::btradio
