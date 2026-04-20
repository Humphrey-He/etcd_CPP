#pragma once

#include <cstdint>
#include <string>

namespace etcdmvp {

struct StorageConfig {
  std::string data_dir;
  uint64_t wal_segment_size = 64ull * 1024ull * 1024ull;
  uint64_t snapshot_threshold_entries = 1000;
  uint64_t watch_history_size = 10000;
  bool fsync_on_write = true;
  uint64_t max_request_size = 1024ull * 1024ull;
  uint64_t max_value_size = 1024ull * 1024ull;
  uint64_t max_concurrent_streams = 1000;
  uint64_t max_watchers = 10000;
};

StorageConfig LoadStorageConfig(int64_t node_id);

} // namespace etcdmvp
