#include "etcdmvp/lease/lease_manager.h"

#include <thread>

namespace etcdmvp {

LeaseManager::LeaseManager(ExpireCallback on_expire) : on_expire_(std::move(on_expire)) {}

LeaseManager::~LeaseManager() { Stop(); }

int64_t LeaseManager::AllocateId() {
  return next_id_.fetch_add(1);
}

int64_t LeaseManager::Grant(int64_t ttl_seconds) {
  int64_t id = AllocateId();
  ApplyGrant(id, ttl_seconds);
  return id;
}

bool LeaseManager::Revoke(int64_t id) {
  std::vector<std::string> keys;
  if (!ApplyRevoke(id, keys)) return false;
  for (const auto& k : keys) {
    if (on_expire_) on_expire_(k);
  }
  return true;
}

bool LeaseManager::KeepAlive(int64_t id, int64_t ttl_seconds, int64_t& out_ttl) {
  return ApplyKeepAlive(id, ttl_seconds, out_ttl);
}

bool LeaseManager::GetTTL(int64_t id, int64_t& out_ttl) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = leases_.find(id);
  if (it == leases_.end()) return false;
  out_ttl = it->second.ttl;
  return true;
}

void LeaseManager::ApplyGrant(int64_t id, int64_t ttl_seconds) {
  if (ttl_seconds <= 0) ttl_seconds = 5;
  std::lock_guard<std::mutex> lock(mu_);
  Lease lease;
  lease.id = id;
  lease.ttl = ttl_seconds;
  lease.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
  leases_[id] = std::move(lease);
}

bool LeaseManager::ApplyKeepAlive(int64_t id, int64_t ttl_seconds, int64_t& out_ttl) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = leases_.find(id);
  if (it == leases_.end()) return false;
  if (ttl_seconds <= 0) ttl_seconds = it->second.ttl;
  it->second.ttl = ttl_seconds;
  it->second.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
  out_ttl = it->second.ttl;
  return true;
}

bool LeaseManager::ApplyRevoke(int64_t id, std::vector<std::string>& out_keys) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = leases_.find(id);
  if (it == leases_.end()) return false;
  out_keys.assign(it->second.keys.begin(), it->second.keys.end());
  for (const auto& k : out_keys) {
    key_leases_.erase(k);
  }
  leases_.erase(it);
  return true;
}

void LeaseManager::AttachKey(int64_t id, const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  if (id == 0) return;
  auto it = leases_.find(id);
  if (it == leases_.end()) return;
  it->second.keys.insert(key);
  key_leases_[key] = id;
}

void LeaseManager::DetachKey(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = key_leases_.find(key);
  if (it == key_leases_.end()) return;
  int64_t id = it->second;
  auto lease_it = leases_.find(id);
  if (lease_it != leases_.end()) {
    lease_it->second.keys.erase(key);
  }
  key_leases_.erase(it);
}

int64_t LeaseManager::LeaseOfKey(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = key_leases_.find(key);
  if (it == key_leases_.end()) return 0;
  return it->second;
}

void LeaseManager::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return;
  running_ = true;
  std::thread([this]() { RunLoop(); }).detach();
}

void LeaseManager::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
  }
}

void LeaseManager::RunLoop() {
  while (true) {
    std::vector<std::string> expired_keys;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!running_) break;
      auto now = std::chrono::steady_clock::now();
      std::vector<int64_t> expired_ids;
      for (const auto& kv : leases_) {
        if (kv.second.expiry <= now) {
          expired_ids.push_back(kv.first);
        }
      }
      for (int64_t id : expired_ids) {
        auto it = leases_.find(id);
        if (it == leases_.end()) continue;
        for (const auto& k : it->second.keys) {
          expired_keys.push_back(k);
          key_leases_.erase(k);
        }
        leases_.erase(it);
      }
    }
    for (const auto& k : expired_keys) {
      if (on_expire_) on_expire_(k);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

} // namespace etcdmvp
