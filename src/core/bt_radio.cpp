#include "core/bt_radio.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>

#include <cstdio>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace pb::btradio {

namespace {
constexpr char kOurVid[] = "VID_0B00";  // our virtual BT adapter
}

bool is_our_virtual(const std::string& instance_id) {
  // Instance IDs look like "USB\VID_0B00&PID_0001\..."; compare case-insensitively.
  std::string upper;
  upper.reserve(instance_id.size());
  for (char c : instance_id) upper.push_back((char)toupper((unsigned char)c));
  return upper.find(kOurVid) != std::string::npos;
}

std::vector<RadioDevice> list_radios() {
  std::vector<RadioDevice> out;
  HDEVINFO set = SetupDiGetClassDevs(&GUID_DEVCLASS_BLUETOOTH, nullptr, nullptr,
                                     DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) return out;
  SP_DEVINFO_DATA di = {};
  di.cbSize = sizeof(di);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &di); ++i) {
    char id[MAX_DEVICE_ID_LEN] = {};
    if (!SetupDiGetDeviceInstanceIdA(set, &di, id, sizeof(id), nullptr)) continue;
    std::string instance(id);
    if (is_our_virtual(instance)) continue;  // never list our virtual adapter

    RadioDevice r;
    r.instance_id = instance;
    char name[256] = {};
    DWORD n = 0, type = 0;
    if (SetupDiGetDeviceRegistryPropertyA(set, &di, SPDRP_FRIENDLYNAME, &type,
                                          (LPBYTE)name, sizeof(name), &n) ||
        SetupDiGetDeviceRegistryPropertyA(set, &di, SPDRP_DEVICEDESC, &type,
                                          (LPBYTE)name, sizeof(name), &n)) {
      r.description = name;
    }
    // Device Manager state: DN_HAS_PROBLEM + CM_PROB_DISABLED = disabled.
    DWORD status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, di.DevInst, 0) == CR_SUCCESS &&
        (status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED)
      r.disabled = true;
    out.push_back(std::move(r));
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

int set_radios_enabled(bool enable, std::string* err) {
  auto radios = list_radios();
  if (radios.empty()) {
    if (err) *err = "no physical bluetooth radio found";
    return 0;
  }
  HDEVINFO set = SetupDiGetClassDevs(&GUID_DEVCLASS_BLUETOOTH, nullptr, nullptr,
                                     DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) {
    if (err) *err = "SetupDiGetClassDevs failed";
    return 0;
  }
  int changed = 0;
  std::string failures;
  for (auto& r : radios) {
    // Find the SP_DEVINFO_DATA for this instance id (indices drift between
    // enumerations, so match by id).
    SP_DEVINFO_DATA it = {};
    it.cbSize = sizeof(it);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &it); ++i) {
      char id[MAX_DEVICE_ID_LEN] = {};
      if (!SetupDiGetDeviceInstanceIdA(set, &it, id, sizeof(id), nullptr)) continue;
      if (r.instance_id == id) {
        found = true;
        break;
      }
    }
    if (!found) {
      failures += r.instance_id + ": not found; ";
      continue;
    }
    SP_PROPCHANGE_PARAMS pcp = {};
    pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pcp.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
    pcp.Scope = DICS_FLAG_GLOBAL;
    pcp.HwProfile = 0;
    if (SetupDiSetClassInstallParams(set, &it, &pcp.ClassInstallHeader, sizeof(pcp)) &&
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &it)) {
      ++changed;
    } else {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lu", GetLastError());
      failures += r.instance_id + ": error " + buf + "; ";
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  if (changed == 0 && err) {
    *err = failures.empty() ? "no device changed (elevation required?)"
                            : "failed: " + failures;
  } else if (!failures.empty() && err) {
    *err = "partial: " + failures;
  }
  return changed;
}

}  // namespace pb::btradio
