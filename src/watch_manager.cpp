#include "etcdmvp/watch/watch_manager.h"

#include <fstream>

namespace etcdmvp {

namespace {
void WriteU64(std::ostream& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xFF));
}

bool ReadU64(std::istream& in, uint64_t& v) {
  v = 0;
  for (int i = 0; i < 8; ++i) {
    char c;
    if (!in.get(c)) return false;
    v |= (static_cast<uint64_t>(static_cast<unsigned char>(c)) << (i * 8));
  }
  return true;
}
}

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

bool WatchManager::SaveHistory(const std::string& path) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  WriteU64(out, static_cast<uint64_t>(history_.size()));
  for (const auto& ev : history_) {
    WriteU64(out, ev.revision);
    out.put(static_cast<char>(ev.type == KvEvent::Type::Put ? 1 : 2));
    WriteU64(out, static_cast<uint64_t>(ev.key.size()));
    WriteU64(out, static_cast<uint64_t>(ev.value.size()));
    out.write(ev.key.data(), static_cast<std::streamsize>(ev.key.size()));
    out.write(ev.value.data(), static_cast<std::streamsize>(ev.value.size()));
  }
  return out.good();
}

bool WatchManager::LoadHistory(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  uint64_t count = 0;
  if (!ReadU64(in, count)) return false;
  history_.clear();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t rev = 0;
    if (!ReadU64(in, rev)) return false;
    char type;
    if (!in.get(type)) return false;
    uint64_t klen = 0;
    uint64_t vlen = 0;
    if (!ReadU64(in, klen) || !ReadU64(in, vlen)) return false;
    std::string key(klen, '\0');
    std::string value(vlen, '\0');
    if (!in.read(&key[0], static_cast<std::streamsize>(klen))) return false;
    if (!in.read(&value[0], static_cast<std::streamsize>(vlen))) return false;
    KvEvent ev;
    ev.revision = rev;
    ev.type = (type == 1) ? KvEvent::Type::Put : KvEvent::Type::Delete;
    ev.key = std::move(key);
    ev.value = std::move(value);
    history_.push_back(std::move(ev));
    last_revision_ = rev;
  }
  TrimHistory();
  return true;
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
