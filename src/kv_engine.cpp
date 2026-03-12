#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/watch/watch_manager.h"

#include <sstream>

namespace etcdmvp {

KvEngine::KvEngine(WatchManager* watch_manager) : watch_manager_(watch_manager) {}

void KvEngine::Apply(const LogEntry& entry) {
  std::istringstream ss(entry.command);
  std::string op;
  ss >> op;
  if (op == "PUT") {
    std::string key;
    std::string value;
    ss >> key;
    std::getline(ss, value);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    uint64_t rev = 0;
    Put(key, value, &rev);
  } else if (op == "DEL") {
    std::string key;
    ss >> key;
    uint64_t rev = 0;
    Delete(key, &rev);
  }
}

bool KvEngine::Put(const std::string& key, const std::string& value, uint64_t* out_revision) {
  KvEvent ev;
  {
    std::lock_guard<std::mutex> lock(mu_);
    kv_[key] = value;
    revision_++;
    ev = KvEvent{KvEvent::Type::Put, key, value, revision_};
    if (out_revision) *out_revision = revision_;
  }
  EmitEvent(ev);
  return true;
}

bool KvEngine::Delete(const std::string& key, uint64_t* out_revision) {
  KvEvent ev;
  bool existed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = kv_.find(key);
    if (it == kv_.end()) {
      return false;
    }
    kv_.erase(it);
    revision_++;
    existed = true;
    ev = KvEvent{KvEvent::Type::Delete, key, "", revision_};
    if (out_revision) *out_revision = revision_;
  }
  if (existed) EmitEvent(ev);
  return existed;
}

bool KvEngine::Get(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = kv_.find(key);
  if (it == kv_.end()) return false;
  value = it->second;
  return true;
}

uint64_t KvEngine::Revision() const {
  std::lock_guard<std::mutex> lock(mu_);
  return revision_;
}

void KvEngine::Snapshot(std::unordered_map<std::string, std::string>& out_kv, uint64_t& out_revision) const {
  std::lock_guard<std::mutex> lock(mu_);
  out_kv = kv_;
  out_revision = revision_;
}

void KvEngine::LoadSnapshot(const std::unordered_map<std::string, std::string>& kv, uint64_t revision) {
  std::lock_guard<std::mutex> lock(mu_);
  kv_ = kv;
  revision_ = revision;
}

void KvEngine::EmitEvent(const KvEvent& ev) {
  if (watch_manager_) watch_manager_->OnEvent(ev);
}

} // namespace etcdmvp
