#include "etcdmvp/compaction/compaction_manager.h"

#include <cstdlib>

namespace etcdmvp {

CompactionManager::CompactionManager() {
  const char* mode_env = std::getenv("ETCD_MVP_AUTO_COMPACTION_MODE");
  if (mode_env) {
    SetAutoCompactionMode(mode_env);
  }

  const char* retention_env = std::getenv("ETCD_MVP_AUTO_COMPACTION_RETENTION");
  if (retention_env) {
    SetAutoCompactionRetention(std::stoull(retention_env));
  }
}

void CompactionManager::SetAutoCompactionMode(const std::string& mode) {
  if (mode == "periodic") {
    mode_ = Mode::Periodic;
  } else if (mode == "revision") {
    mode_ = Mode::Revision;
  } else {
    mode_ = Mode::Disabled;
  }
}

void CompactionManager::SetAutoCompactionRetention(uint64_t retention) {
  if (mode_ == Mode::Periodic) {
    retention_hours_ = retention;
  } else if (mode_ == Mode::Revision) {
    retention_revisions_ = retention;
  }
}

bool CompactionManager::ShouldCompact(uint64_t current_revision, uint64_t oldest_revision) const {
  if (mode_ == Mode::Disabled) return false;

  if (mode_ == Mode::Revision) {
    return (current_revision - oldest_revision) > retention_revisions_;
  }

  return false;
}

uint64_t CompactionManager::CalculateCompactRevision(uint64_t current_revision, uint64_t oldest_revision) const {
  if (mode_ == Mode::Revision) {
    if (current_revision > retention_revisions_) {
      return current_revision - retention_revisions_;
    }
  }

  return oldest_revision;
}

} // namespace etcdmvp
