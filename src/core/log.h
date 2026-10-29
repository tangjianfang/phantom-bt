#pragma once
// 分级日志：默认写 stderr，可替换 sink（GUI/管道复用）
#include <functional>
#include <string_view>

namespace pb {

enum class LogLevel : int { Debug = 0, Info, Warn, Error };

void log_set_level(LogLevel lv);
void log_set_sink(std::function<void(LogLevel, std::string_view line)>);
void log_write(LogLevel lv, std::string_view msg);

}  // namespace pb

#define PB_LOG(lv, msg) pb::log_write((lv), (msg))
#define PB_LOG_DEBUG(msg) pb::log_write(pb::LogLevel::Debug, (msg))
#define PB_LOG_INFO(msg) pb::log_write(pb::LogLevel::Info, (msg))
#define PB_LOG_WARN(msg) pb::log_write(pb::LogLevel::Warn, (msg))
#define PB_LOG_ERROR(msg) pb::log_write(pb::LogLevel::Error, (msg))
