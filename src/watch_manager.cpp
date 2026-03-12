#include "etcdmvp/watch/watch_manager.h"

namespace etcdmvp {

WatchManager::WatchManager(uint64_t history_limit) : history_limit_(history_limit) {}

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
    last_revision_ = ev.revision;
    history_.push_back(ev);
    TrimHistory();

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

bool WatchManager::GetHistory(uint64_t start_revision, std::vector<KvEvent>& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  out.clear();
  if (!history_.empty()) {
    uint64_t oldest = history_.front().revision;
    if (start_revision < oldest) return false;
  }
  for (const auto& ev : history_) {
    if (ev.revision >= start_revision) out.push_back(ev);
  }
  return true;
}

uint64_t WatchManager::OldestRevision() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!history_.empty()) return history_.front().revision;
  return last_revision_ + 1;
}

void WatchManager::TrimHistory() {
  if (history_limit_ == 0) {
    history_.clear();
    return;
  }
  while (history_.size() > history_limit_) {
    history_.pop_front();
  }
}

} // namespace etcdmvp
