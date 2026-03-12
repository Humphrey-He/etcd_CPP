#pragma once

#include "etcdmvp/raft/raft.h"
#include "etcdmvp/storage/config.h"

#include <mutex>
#include <string>
#include <vector>

namespace etcdmvp {

class WalFile : public IWal {
public:
  explicit WalFile(const StorageConfig& config);

  void SetSnapshotBase(uint64_t index, uint64_t term);

  bool Append(const LogEntry& entry) override;
  bool SaveHardState(const HardState& hs) override;
  bool Load(std::vector<LogEntry>& out_log, HardState& out_hs) override;

  bool PurgeUpTo(uint64_t index);

  std::string LastErrorCode() const;
  std::string LastErrorMessage() const;

private:
  bool AppendEntry(const LogEntry& entry);
  bool EnsureSegment(uint64_t entry_index, size_t record_size);
  bool ReadSegments(std::vector<LogEntry>& out_log);

  void SetError(const std::string& code, const std::string& message);

  StorageConfig config_;
  std::string wal_dir_;
  std::string hs_path_;

  uint64_t snapshot_base_index_ = 0;
  uint64_t snapshot_base_term_ = 0;

  uint64_t current_segment_start_ = 0;
  std::string current_segment_path_;
  uint64_t current_segment_size_ = 0;

  std::string last_error_code_;
  std::string last_error_message_;

  std::mutex mu_;
};

} // namespace etcdmvp
