#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/watch/watch_manager.h"

#include <sstream>

namespace etcdmvp {

namespace {
bool ReadU8(const std::string& data, size_t& off, uint8_t& out) {
  if (off + 1 > data.size()) return false;
  out = static_cast<uint8_t>(data[off]);
  off += 1;
  return true;
}

bool ReadU32(const std::string& data, size_t& off, uint32_t& out) {
  if (off + 4 > data.size()) return false;
  out = 0;
  for (int i = 0; i < 4; ++i) {
    out |= (static_cast<uint32_t>(static_cast<unsigned char>(data[off + i])) << (i * 8));
  }
  off += 4;
  return true;
}

bool ReadU64(const std::string& data, size_t& off, uint64_t& out) {
  if (off + 8 > data.size()) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= (static_cast<uint64_t>(static_cast<unsigned char>(data[off + i])) << (i * 8));
  }
  off += 8;
  return true;
}
}

KvEngine::KvEngine(WatchManager* watch_manager, LeaseManager* lease_manager)
    : watch_manager_(watch_manager), lease_manager_(lease_manager) {}

// Apply a committed Raft log entry to the KV state machine
void KvEngine::Apply(const LogEntry& entry) {
  // Handle transaction batch (TXN1 format)
  if (entry.command.rfind("TXN1", 0) == 0) {
    size_t off = 4;
    uint32_t op_count = 0;
    if (!ReadU32(entry.command, off, op_count)) return;
    for (uint32_t i = 0; i < op_count; ++i) {
      uint8_t type = 0;
      uint32_t klen = 0;
      uint32_t vlen = 0;
      uint64_t lease = 0;
      if (!ReadU8(entry.command, off, type)) return;
      if (!ReadU32(entry.command, off, klen)) return;
      if (off + klen > entry.command.size()) return;
      std::string key = entry.command.substr(off, klen);
      off += klen;
      if (!ReadU32(entry.command, off, vlen)) return;
      if (off + vlen > entry.command.size()) return;
      std::string value = entry.command.substr(off, vlen);
      off += vlen;
      if (!ReadU64(entry.command, off, lease)) return;

      if (type == 1) {
        uint64_t rev = 0;
        PutWithLease(key, value, static_cast<int64_t>(lease), &rev);
      } else if (type == 2) {
        uint64_t rev = 0;
        Delete(key, &rev);
      }
    }
    return;
  }

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
  } else if (op == "LEASE_GRANT") {
    int64_t id = 0;
    int64_t ttl = 0;
    ss >> id >> ttl;
    if (lease_manager_) lease_manager_->ApplyGrant(id, ttl);
  } else if (op == "LEASE_KEEPALIVE") {
    int64_t id = 0;
    int64_t ttl = 0;
    ss >> id >> ttl;
    int64_t out = 0;
    if (lease_manager_) lease_manager_->ApplyKeepAlive(id, ttl, out);
  } else if (op == "LEASE_REVOKE") {
    int64_t id = 0;
    ss >> id;
    if (lease_manager_) {
      std::vector<std::string> keys;
      if (lease_manager_->ApplyRevoke(id, keys)) {
        for (const auto& key : keys) {
          uint64_t rev = 0;
          Delete(key, &rev);
        }
      }
    }
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

bool KvEngine::Compact(uint64_t revision) {
  std::lock_guard<std::mutex> lock(mu_);

  if (revision <= compact_revision_) return false;
  if (revision > revision_) return false;

  for (auto& kv : history_) {
    auto& versions = kv.second;
    auto it = versions.begin();
    while (it != versions.end()) {
      if (it->revision < revision) {
        it = versions.erase(it);
      } else {
        ++it;
      }
    }
  }

  compact_revision_ = revision;
  return true;
}

uint64_t KvEngine::CompactRevision() const {
  std::lock_guard<std::mutex> lock(mu_);
  return compact_revision_;
}

} // namespace etcdmvp
