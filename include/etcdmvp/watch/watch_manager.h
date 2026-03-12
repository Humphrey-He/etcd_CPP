#pragma once

#include "etcdmvp/kv/kv_engine.h"

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

  uint64_t Register(const WatchRequest& req, Callback cb);
  void Unregister(uint64_t id);
  void OnEvent(const KvEvent& ev);

private:
  struct Watcher {
    WatchRequest req;
    Callback cb;
  };

  std::mutex mu_;
  uint64_t next_id_ = 1;
  std::unordered_map<uint64_t, Watcher> watchers_;
};

} // namespace etcdmvp
