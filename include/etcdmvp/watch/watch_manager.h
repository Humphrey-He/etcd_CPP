#pragma once

#include "etcdmvp/kv/kv_engine.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace etcdmvp {

struct WatchRequest {
  uint64_t id = 0;
  std::string key;
  bool prefix = false;
  uint64_t start_revision = 0;
};

class WatchManager {
public:
  using Callback = std::function<void(const KvEvent&)>;

  explicit WatchManager(uint64_t history_limit = 10000);

  uint64_t Register(const WatchRequest& req, Callback cb);
  void Unregister(uint64_t id);
  void OnEvent(const KvEvent& ev);

  bool GetHistory(uint64_t start_revision, std::vector<KvEvent>& out) const;
  uint64_t OldestRevision() const;

private:
  struct Watcher {
    WatchRequest req;
    Callback cb;
  };

  void TrimHistory();

  uint64_t history_limit_ = 10000;
  mutable std::mutex mu_;
  uint64_t next_id_ = 1;
  std::unordered_map<uint64_t, Watcher> watchers_;
  std::deque<KvEvent> history_;
  uint64_t last_revision_ = 0;
};

} // namespace etcdmvp
