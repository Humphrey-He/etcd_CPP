#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace etcdmvp {

class SnapshotFile {
public:
  explicit SnapshotFile(const std::string& path);

  bool Save(const std::unordered_map<std::string, std::string>& kv, uint64_t revision);
  bool Load(std::unordered_map<std::string, std::string>& kv, uint64_t& revision);

private:
  std::string path_;
};

} // namespace etcdmvp
