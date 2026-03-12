#include "etcdmvp/watch/watch_manager.h"

namespace etcdmvp {

uint64_t WatchManager::Register(const WatchRequest& req, Callback cb) {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t id = next_id_++;
  WatchRequest r = req;
  r.id = id;
  watchers_[id] = Watcher{r, std::move(cb)};
  return id;
}

void WatchManager::Unregister(uint64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  watchers_.erase(id);
}

void WatchManager::OnEvent(const KvEvent& ev) {
  std::vector<Callback> cbs;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& kv : watchers_) {
      const WatchRequest& r = kv.second.req;
      if (ev.revision < r.start_revision) continue;
      if (!r.prefix) {
        if (r.key == ev.key) cbs.push_back(kv.second.cb);
      } else {
        if (ev.key.rfind(r.key, 0) == 0) cbs.push_back(kv.second.cb);
      }
    }
  }
  for (auto& cb : cbs) {
    cb(ev);
  }
}

} // namespace etcdmvp
