#include "test/test_framework.h"

#include <string>

#include "core/bt_radio.h"

using namespace pb;

PB_TEST(btr_our_virtual_exclusion) {
  // Our virtual adapter must be recognized in every instance-id shape.
  PB_CHECK(pb::btradio::is_our_virtual("USB\\VID_0B00&PID_0001\\1234"));
  PB_CHECK(pb::btradio::is_our_virtual("usb\\vid_0b00&pid_0001\\5&2ab8f6d&0&10"));
  // Physical radios and unrelated devices are not ours.
  PB_CHECK(!pb::btradio::is_our_virtual("USB\\VID_8087&PID_0026\\5&2ab8f6d&0&10"));
  PB_CHECK(!pb::btradio::is_our_virtual("USB\\VID_0A12&PID_0001\\x"));
  PB_CHECK(!pb::btradio::is_our_virtual(""));
  // Prefix-ish but different vendor must not match (0B00 vs 0B05).
  PB_CHECK(!pb::btradio::is_our_virtual("USB\\VID_0B05&PID_0011\\x"));
}

// Real-machine shape check (listing is allowed unelevated). We never toggle
// state here — disabling the user's radio belongs to explicit user action.
PB_TEST(btr_list_radios_shape) {
  auto radios = pb::btradio::list_radios();
  for (auto& r : radios) {
    PB_CHECK(!r.instance_id.empty());
    PB_CHECK(r.instance_id.find('\\') != std::string::npos);
    PB_CHECK(!pb::btradio::is_our_virtual(r.instance_id));
  }
}
