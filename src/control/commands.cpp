#include "control/commands.h"

#include <sstream>

#include "core/bytes.h"

namespace pb::control {

std::string usage() {
  return "commands: load-hid <json> | list | attach <busid> | detach <busid> | "
         "inject <busid> <hex> | status | help";
}

static std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream ss(line);
  std::string tok;
  while (ss >> tok) out.push_back(tok);
  return out;
}

std::vector<std::string> execute(sim::Simulator& sim, const std::string& line) {
  auto args = split(line);
  if (args.empty()) return {"ERR empty command"};
  const std::string& cmd = args[0];
  try {
    if (cmd == "load-hid" && args.size() == 2) {
      return {"OK " + sim.cmd_load_hid_profile(args[1])};
    }
    if (cmd == "load-bt") {
      return {"OK " + sim.cmd_load_bt_adapter()};
    }
    if (cmd == "ble-advertise" && args.size() == 2) {
      return {sim.cmd_ble_advertise(args[1])};
    }
    if (cmd == "ble-stop" && args.size() == 2) {
      return {"OK " + sim.cmd_ble_stop(args[1])};
    }
    if (cmd == "ble-list") {
      return {"OK", sim.cmd_ble_list(), "OK-END"};
    }
    if (cmd == "ble-type" && args.size() == 3) {
      return {"OK " + sim.cmd_ble_type(args[1], args[2])};
    }
    if (cmd == "list") {
      std::vector<std::string> out;
      std::string l = sim.cmd_list();
      if (!l.empty()) {
        std::istringstream ss(l);
        std::string row;
        while (std::getline(ss, row))
          if (!row.empty()) out.push_back("DEV " + row);
      }
      out.push_back("OK-END");
      return out;
    }
    if (cmd == "attach" && args.size() == 2) {
      return {"OK " + sim.cmd_attach(args[1])};
    }
    if (cmd == "detach" && args.size() == 2) {
      return {"OK " + sim.cmd_detach(args[1])};
    }
    if (cmd == "inject" && args.size() == 3) {
      auto bytes = from_hex(args[2]);
      return {"OK " + sim.cmd_inject(args[1], bytes)};
    }
    if (cmd == "status") {
      return {"OK " + sim.cmd_status()};
    }
    if (cmd == "help") {
      return {"OK " + usage()};
    }
    return {"ERR unknown command '" + cmd + "' (" + usage() + ")"};
  } catch (const std::exception& e) {
    return {"ERR " + std::string(e.what())};
  }
}

}  // namespace pb::control
