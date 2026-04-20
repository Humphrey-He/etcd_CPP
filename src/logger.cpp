#include "etcdmvp/logging/logger.h"

#include <cstdlib>
#include <iomanip>
#include <mutex>

namespace etcdmvp {

namespace {
std::mutex g_log_mutex;

LogLevel ParseLogLevel(const char* env) {
  if (!env) return LogLevel::INFO;
  std::string s(env);
  if (s == "DEBUG") return LogLevel::DEBUG;
  if (s == "INFO") return LogLevel::INFO;
  if (s == "WARN") return LogLevel::WARN;
  if (s == "ERROR") return LogLevel::ERROR;
  return LogLevel::INFO;
}
} // namespace

Logger::Logger() {
  const char* env = std::getenv("ETCD_MVP_LOG_LEVEL");
  level_ = ParseLogLevel(env);
}

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

void Logger::SetLevel(LogLevel level) {
  level_ = level;
}

LogLevel Logger::GetLevel() const {
  return level_;
}

void Logger::Log(LogLevel level, const std::string& module, const std::string& event,
                 const std::string& trace_id, const std::string& message) {
  if (level < level_) return;

  std::lock_guard<std::mutex> lock(g_log_mutex);

  std::ostringstream ss;
  ss << "{"
     << "\"ts\":" << CurrentTimestamp() << ","
     << "\"level\":\"" << LevelToString(level) << "\","
     << "\"module\":\"" << module << "\","
     << "\"event\":\"" << event << "\","
     << "\"trace_id\":\"" << trace_id << "\","
     << "\"message\":\"" << message << "\""
     << "}";

  std::cout << ss.str() << std::endl;
}

std::string Logger::LevelToString(LogLevel level) const {
  switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO: return "INFO";
    case LogLevel::WARN: return "WARN";
    case LogLevel::ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

int64_t Logger::CurrentTimestamp() const {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

} // namespace etcdmvp
