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
//  snoop <path>|off       → OK（重定向/关闭 HCI 抓包）
//  status                 → OK usbip-port=<p> devices=<n>
//  help                   → 用法
std::vector<std::string> execute(sim::Simulator& sim, const std::string& line);
std::string usage();

// 引号感知的切分：双引号内的空格属于同一个 token。无引号时由 execute 再把
// 路径 / ble-type 文本的剩余 token 用空格拼回去。
std::vector<std::string> tokenize(const std::string& line);
// 从 args[from] 起用单个空格拼回（路径或 ble-type 文本）。
std::string join_tokens(const std::vector<std::string>& args, size_t from);

}  // namespace pb::control
