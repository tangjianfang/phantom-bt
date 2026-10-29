#pragma once
// 文本命令协议（CLI/GUI/命名管道共享）
#include <string>
#include <vector>

#include "sim/simulator.h"

namespace pb::control {

// 语法（一行一条，空格分隔）：
//  load-hid <path.json>   → OK <busid>
//  load-bt                → OK <busid>
//  list                   → 每设备一行 + OK-END
//  attach <busid>         → OK
//  detach <busid>         → OK
//  inject <busid> <hex>   → OK
//  status                 → OK usbip-port=<p> devices=<n>
//  help                   → 用法
std::vector<std::string> execute(sim::Simulator& sim, const std::string& line);
std::string usage();

}  // namespace pb::control
