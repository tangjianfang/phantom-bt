#include "test/test_framework.h"
#include "core/log.h"

#include <stdexcept>
#include <string>
#include <vector>

PB_TEST(log_sink_receives_lines_and_level_filter) {
  std::vector<std::string> lines;
  std::vector<pb::LogLevel> levels;
  pb::log_set_sink([&](pb::LogLevel lv, std::string_view s) {
    lines.push_back(std::string(s));
    levels.push_back(lv);
  });
  pb::log_set_level(pb::LogLevel::Debug);
  PB_LOG_INFO("hello");
  PB_LOG_DEBUG("dbg");
  PB_LOG_ERROR("bad");

  pb::log_set_level(pb::LogLevel::Error);
  PB_LOG_INFO("filtered-out");  // 低于级别被滤除
  PB_LOG_WARN("filtered-too");
  PB_LOG_ERROR("kept");

  pb::log_set_sink(nullptr);  // 恢复默认，避免污染其他测试
  pb::log_set_level(pb::LogLevel::Info);

  PB_CHECK_EQ(lines.size(), 4u);
  PB_CHECK(levels[0] == pb::LogLevel::Info);
  PB_CHECK(levels[1] == pb::LogLevel::Debug);
  PB_CHECK(levels[3] == pb::LogLevel::Error);
  PB_CHECK(lines[0].find("hello") != std::string::npos);
  PB_CHECK(lines[1].find("dbg") != std::string::npos);
  PB_CHECK(lines[2].find("bad") != std::string::npos);
  PB_CHECK(lines[3].find("kept") != std::string::npos);
  PB_CHECK(lines[0].find("INFO") != std::string::npos);
}

PB_TEST(log_sink_exception_does_not_propagate) {
  pb::log_set_sink([](pb::LogLevel, std::string_view) { throw std::runtime_error("boom"); });
  bool no_crash = true;
  try {
    PB_LOG_INFO("trigger");
  } catch (...) {
    no_crash = false;
  }
  pb::log_set_sink(nullptr);
  PB_CHECK(no_crash);
}
