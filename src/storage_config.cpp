#include "etcdmvp/storage/config.h"

#include <cstdlib>

namespace etcdmvp {

namespace {
uint64_t ParseU64(const char* v, uint64_t fallback) {
  if (!v) return fallback;
  char* end = nullptr;
  unsigned long long value = std::strtoull(v, &end, 10);
  if (end == v) return fallback;
  return static_cast<uint64_t>(value);
}

bool ParseBool(const char* v, bool fallback) {
  if (!v) return fallback;
  if (std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "TRUE") return true;
  if (std::string(v) == "0" || std::string(v) == "false" || std::string(v) == "FALSE") return false;
  return fallback;
}
}

StorageConfig LoadStorageConfig(int64_t node_id) {
  StorageConfig cfg;
  const char* data_dir = std::getenv("ETCD_MVP_DATA_DIR");
  if (data_dir) {
    cfg.data_dir = data_dir;
    if (!cfg.data_dir.empty() && cfg.data_dir.back() != '/' && cfg.data_dir.back() != '\\') {
      cfg.data_dir += "/node" + std::to_string(node_id);
    }
  } else {
    cfg.data_dir = "data/node" + std::to_string(node_id);
  }

  cfg.wal_segment_size = ParseU64(std::getenv("ETCD_MVP_WAL_SEGMENT_SIZE"), cfg.wal_segment_size);
  cfg.snapshot_threshold_entries = ParseU64(std::getenv("ETCD_MVP_SNAPSHOT_THRESHOLD"),
                                            cfg.snapshot_threshold_entries);
  cfg.watch_history_size = ParseU64(std::getenv("ETCD_MVP_WATCH_HISTORY_SIZE"), cfg.watch_history_size);
  cfg.fsync_on_write = ParseBool(std::getenv("ETCD_MVP_FSYNC"), cfg.fsync_on_write);
  return cfg;
}

} // namespace etcdmvp
