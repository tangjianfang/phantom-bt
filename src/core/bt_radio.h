#pragma once
// Real (physical) Bluetooth radio control: enumerate / disable / enable via
// SetupAPI — the programmatic equivalent of Device Manager's "disable device".
// Shared by the GUI global-settings page and the CLI. Requires elevation to
// change state; listing works unelevated.
#include <string>
#include <vector>

namespace pb::btradio {

struct RadioDevice {
  std::string instance_id;  // e.g. USB\VID_8087&PID_0026\5&2ab8f6d&0&10
  std::string description;  // friendly name (may be empty)
  bool disabled = false;    // current Device Manager state
};

// True for our own virtual adapter instances (must never be touched).
bool is_our_virtual(const std::string& instance_id);  // VID_0B00 exclusion

// All present physical BT radios (Bluetooth device class, virtual excluded).
std::vector<RadioDevice> list_radios();

// Enable/disable every enumerated physical radio. Returns how many devices
// the class installer accepted; *err explains a total failure (typically
// "requires elevation"). Partial failures are reported in err as well.
int set_radios_enabled(bool enable, std::string* err);

}  // namespace pb::btradio
