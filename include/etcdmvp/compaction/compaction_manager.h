#pragma once

#include <cstdint>
#include <string>

namespace etcdmvp {

class CompactionManager {
public:
  CompactionManager();

  void SetAutoCompactionMode(const std::string& mode);
  void SetAutoCompactionRetention(uint64_t retention);

  bool ShouldCompact(uint64_t current_revision, uint64_t oldest_revision) const;
  uint64_t CalculateCompactRevision(uint64_t current_revision, uint64_t oldest_revision) const;

private:
  enum class Mode { Disabled, Periodic, Revision };

  Mode mode_ = Mode::Disabled;
  uint64_t retention_revisions_ = 10000;
  uint64_t retention_hours_ = 1;
};

} // namespace etcdmvp
