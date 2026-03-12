#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace etcdmvp::testutil {

inline std::string TempDir(const std::string& prefix) {
  auto base = std::filesystem::temp_directory_path();
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path dir = base / (prefix + "_" + std::to_string(now));
  std::filesystem::create_directories(dir);
  return dir.string();
}

inline void RemoveAll(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

} // namespace etcdmvp::testutil
