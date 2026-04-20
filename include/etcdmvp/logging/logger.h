#pragma once

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

namespace etcdmvp {

enum class LogLevel {
  DEBUG = 0,
  INFO = 1,
  WARN = 2,
  ERROR = 3
};

class Logger {
public:
  static Logger& Instance();

  void SetLevel(LogLevel level);
  LogLevel GetLevel() const;

  void Log(LogLevel level, const std::string& module, const std::string& event,
           const std::string& trace_id, const std::string& message);

private:
  Logger();
  LogLevel level_;

  std::string LevelToString(LogLevel level) const;
  int64_t CurrentTimestamp() const;
};

#define LOG_DEBUG(module, event, trace_id, message) \
  etcdmvp::Logger::Instance().Log(etcdmvp::LogLevel::DEBUG, module, event, trace_id, message)

#define LOG_INFO(module, event, trace_id, message) \
  etcdmvp::Logger::Instance().Log(etcdmvp::LogLevel::INFO, module, event, trace_id, message)

#define LOG_WARN(module, event, trace_id, message) \
  etcdmvp::Logger::Instance().Log(etcdmvp::LogLevel::WARN, module, event, trace_id, message)

#define LOG_ERROR(module, event, trace_id, message) \
  etcdmvp::Logger::Instance().Log(etcdmvp::LogLevel::ERROR, module, event, trace_id, message)

} // namespace etcdmvp
