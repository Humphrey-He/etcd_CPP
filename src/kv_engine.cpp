#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/watch/watch_manager.h"

#include <sstream>

namespace etcdmvp {

KvEngine::KvEngine(WatchManager* watch_manager, LeaseManager* lease_manager)
    : watch_manager_(watch_manager), lease_manager_(lease_manager) {}

void KvEngine::Apply(const LogEntry& entry) {
  std::istringstream ss(entry.command);
  std::string op;
  ss >> op;
  if (op == "PUT") {
    std::string key;
    std::string value;
    int64_t lease_id = 0;
    ss >> key;
    ss >> lease_id;
    std::getline(ss, value);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    uint64_t rev = 0;
    PutWithLease(key, value, lease_id, &rev);
  } else if (op == "DEL") {
    std::string key;
    ss >> key;
    uint64_t rev = 0;
    Delete(key, &rev);
  }
}

bool KvEngine::Put(const std::string& key, const std::string& value, uint64_t* out_revision) {
  return PutWithLease(key, value, 0, out_revision);
}

bool KvEngine::PutWithLease(const std::string& key,
                            const std::string& value,
                            int64_t lease_id,
                            uint64_t* out_revision) {
  KvEvent ev;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto& versions = history_[key];
    if (!versions.empty() && lease_manager_) {
      int64_t existing = versions.back().lease_id;
      if (existing != 0 && existing != lease_id) {
        lease_manager_->DetachKey(key);
      }
    }
    revision_++;
    versions.push_back(Version{revision_, value, false, lease_id});
    ev = KvEvent{KvEvent::Type::Put, key, value, revision_};
    if (out_revision) *out_revision = revision_;
    if (lease_manager_ && lease_id != 0) {
      lease_manager_->AttachKey(lease_id, key);
    }
  }
  EmitEvent(ev);
  if (snapshot_hook_) snapshot_hook_(ev.revision);
  return true;
}

bool KvEngine::Delete(const std::string& key, uint64_t* out_revision) {
  KvEvent ev;
  bool existed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = history_.find(key);
    if (it == history_.end() || it->second.empty()) {
      return false;
    }
    if (!it->second.empty() && it->second.back().deleted) {
      return false;
    }
    revision_++;
    it->second.push_back(Version{revision_, "", true, 0});
    existed = true;
    ev = KvEvent{KvEvent::Type::Delete, key, "", revision_};
    if (out_revision) *out_revision = revision_;
    if (lease_manager_) {
      lease_manager_->DetachKey(key);
    }
  }
  if (existed) EmitEvent(ev);
  if (snapshot_hook_) snapshot_hook_(ev.revision);
  return existed;
}

bool KvEngine::Get(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = history_.find(key);
  if (it == history_.end() || it->second.empty()) return false;
  const auto& v = it->second.back();
  if (v.deleted) return false;
  value = v.value;
  return true;
}

bool KvEngine::GetMetadata(const std::string& key,
                           uint64_t& create_rev,
                           uint64_t& mod_rev,
                           uint64_t& version,
                           int64_t& lease) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = history_.find(key);
  if (it == history_.end() || it->second.empty()) return false;
  const auto& versions = it->second;
  create_rev = versions.front().revision;
  mod_rev = versions.back().revision;
  version = static_cast<uint64_t>(versions.size());
  lease = versions.back().lease_id;
  if (versions.back().deleted) return false;
  return true;
}

uint64_t KvEngine::Revision() const {
  std::lock_guard<std::mutex> lock(mu_);
  return revision_;
}

void KvEngine::Snapshot(std::unordered_map<std::string, std::string>& out_kv, uint64_t& out_revision) const {
  std::lock_guard<std::mutex> lock(mu_);
  out_kv.clear();
  for (const auto& kv : history_) {
    if (!kv.second.empty() && !kv.second.back().deleted) {
      out_kv[kv.first] = kv.second.back().value;
    }
  }
  out_revision = revision_;
}

void KvEngine::LoadSnapshot(const std::unordered_map<std::string, std::string>& kv, uint64_t revision) {
  std::lock_guard<std::mutex> lock(mu_);
  history_.clear();
  for (const auto& it : kv) {
    history_[it.first].push_back(Version{revision, it.second, false, 0});
  }
  revision_ = revision;
}

void KvEngine::SetSnapshotHook(std::function<void(uint64_t)> hook) {
  snapshot_hook_ = std::move(hook);
}

void KvEngine::EmitEvent(const KvEvent& ev) {
  if (watch_manager_) watch_manager_->OnEvent(ev);
}

} // namespace etcdmvp
