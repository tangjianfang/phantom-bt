#include "core/log.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include <windows.h>

namespace pb {

namespace {
std::mutex g_mutex;
LogLevel g_level = LogLevel::Info;
std::function<void(LogLevel, std::string_view)> g_sink;
const char* level_name(LogLevel lv) {
  switch (lv) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}
}  // namespace

void log_set_level(LogLevel lv) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_level = lv;
}

void log_set_sink(std::function<void(LogLevel, std::string_view)> sink) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_sink = std::move(sink);
}

void log_write(LogLevel lv, std::string_view msg) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (static_cast<int>(lv) < static_cast<int>(g_level)) return;

  SYSTEMTIME st;
  GetLocalTime(&st);
  char ts[32];
  std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);

  std::string line;
  line.reserve(msg.size() + 32);
  line += '[';
  line += level_name(lv);
  line += "][";
  line += ts;
  line += "] ";
  line.append(msg.data(), msg.size());

  if (g_sink) {
    try {
      g_sink(lv, line);
    } catch (...) {
      // sink 故障不得影响调用方；忽略
    }
  } else {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
  }
}

}  // namespace pb
