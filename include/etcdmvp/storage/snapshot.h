#pragma once

#include "etcdmvp/storage/config.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace etcdmvp {

class SnapshotFile {
public:
  explicit SnapshotFile(const StorageConfig& config);

  bool Save(const std::unordered_map<std::string, std::string>& kv,
            uint64_t revision,
            uint64_t last_term,
            const std::string& metadata_json);
  bool Load(std::unordered_map<std::string, std::string>& kv,
            uint64_t& revision,
            uint64_t& last_term,
            std::string& metadata_json);

  std::string LastErrorCode() const;
  std::string LastErrorMessage() const;

private:
  void SetError(const std::string& code, const std::string& message);

  StorageConfig config_;
  std::string path_;
  std::string last_error_code_;
  std::string last_error_message_;
};

} // namespace etcdmvp
