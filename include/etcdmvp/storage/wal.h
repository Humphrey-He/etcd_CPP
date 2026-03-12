#pragma once

#include "etcdmvp/raft/raft.h"

#include <mutex>
#include <string>
#include <vector>

namespace etcdmvp {

class WalFile : public IWal {
public:
  explicit WalFile(const std::string& path_prefix);

  bool Append(const LogEntry& entry) override;
  bool SaveHardState(const HardState& hs) override;
  bool Load(std::vector<LogEntry>& out_log, HardState& out_hs) override;

private:
  bool AppendRecord(char type, const std::string& payload);
  bool ReadRecords(std::vector<LogEntry>& out_log, HardState& out_hs);

  std::string wal_path_;
  std::string hs_path_;
  std::mutex mu_;
};

} // namespace etcdmvp
