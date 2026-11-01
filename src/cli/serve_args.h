#pragma once
// phantombt serve 参数解析（bug cli-stoi）。
// --port / --exit-after-ms 等取值选项的解析必须安全：缺值、空串、非数字、越界
// 一律置 ok=false 并给出可读 error，由调用方打印用法并以非零码退出 —— 绝不抛
// std::stoi 的 std::invalid_argument / std::out_of_range（未捕获异常会触发
// Windows 错误报告模态框，把命令行工具挂死在弹窗上）。
// 头文件内联实现：cli/main.cpp 与单测共用同一份逻辑。
#include <cstdint>
#include <string>
#include <vector>

namespace pb::cli {

struct ServeArgs {
  bool ok = true;     // false = 用法错误（error 给出可读原因）
  std::string error;  // ok=false 时的原因（如 "--port requires a value"）
  uint16_t port = 3240;
  int exit_after_ms = 0;
  bool load_bt = false;
  bool attach = false;
  std::vector<std::string> loads;
  std::vector<std::string> bles;
  std::string probe;
};

// 纯十进制无符号解析（带上界，防乘法溢出）：空串/非数字/越上界 → false，不抛异常
inline bool parse_uint_bound(const std::string& s, unsigned long long max,
                             unsigned long long* out) {
  if (s.empty()) return false;
  unsigned long long v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    unsigned long long d = (unsigned long long)(c - '0');
    if (v > (max - d) / 10ull) return false;  // v*10+d 必越上界
    v = v * 10ull + d;
  }
  *out = v;
  return true;
}

// 解析 serve 之后的参数（不含 "serve" 本身）。任何用法错误都停在第一处：
// ok=false 时其余字段是解析到出错为止的部分结果，调用方只应打印 usage 并退出。
inline ServeArgs parse_serve_args(const std::vector<std::string>& args) {
  ServeArgs r;
  for (size_t i = 0; i < args.size() && r.ok; ++i) {
    const std::string& a = args[i];
    // 取值选项的下一个 token；缺失即用法错误（返回空串且 ok=false）
    auto next = [&](const char* flag) -> std::string {
      if (i + 1 >= args.size()) {
        r.ok = false;
        r.error = std::string(flag) + " requires a value";
        return "";
      }
      return args[++i];
    };
    if (a == "--port") {
      std::string v = next("--port");
      if (r.ok) {
        unsigned long long n = 0;
        if (!parse_uint_bound(v, 65535ull, &n)) {
          r.ok = false;
          r.error = "invalid --port value '" + v + "'";
        } else {
          r.port = (uint16_t)n;
        }
      }
    } else if (a == "--exit-after-ms") {
      std::string v = next("--exit-after-ms");
      if (r.ok) {
        unsigned long long n = 0;
        if (!parse_uint_bound(v, 2147483647ull, &n)) {
          r.ok = false;
          r.error = "invalid --exit-after-ms value '" + v + "'";
        } else {
          r.exit_after_ms = (int)n;
        }
      }
    } else if (a == "--load") {
      std::string v = next("--load");
      if (r.ok) r.loads.push_back(v);
    } else if (a == "--load-bt") {
      r.load_bt = true;
    } else if (a == "--ble") {
      std::string v = next("--ble");
      if (r.ok) r.bles.push_back(v);
    } else if (a == "--attach") {
      r.attach = true;
    } else if (a == "--probe") {
      std::string v = next("--probe");
      if (r.ok) r.probe = v;
    } else {
      r.ok = false;
      r.error = "unknown flag " + a;
    }
  }
  return r;
}

}  // namespace pb::cli
