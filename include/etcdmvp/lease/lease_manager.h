#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace etcdmvp {

class LeaseManager {
public:
  using ExpireCallback = std::function<void(const std::string& key)>;

  explicit LeaseManager(ExpireCallback on_expire);
  ~LeaseManager();

  int64_t AllocateId();
  int64_t Grant(int64_t ttl_seconds);
  bool Revoke(int64_t id);
  bool KeepAlive(int64_t id, int64_t ttl_seconds, int64_t& out_ttl);
  bool GetTTL(int64_t id, int64_t& out_ttl) const;

  void ApplyGrant(int64_t id, int64_t ttl_seconds);
  bool ApplyKeepAlive(int64_t id, int64_t ttl_seconds, int64_t& out_ttl);
  bool ApplyRevoke(int64_t id, std::vector<std::string>& out_keys);

  void AttachKey(int64_t id, const std::string& key);
  void DetachKey(const std::string& key);
  int64_t LeaseOfKey(const std::string& key) const;

  void Start();
  void Stop();

private:
  struct Lease {
    int64_t id = 0;
    int64_t ttl = 0;
    std::chrono::steady_clock::time_point expiry;
    std::unordered_set<std::string> keys;
  };

  void RunLoop();

  mutable std::mutex mu_;
  std::unordered_map<int64_t, Lease> leases_;
  std::unordered_map<std::string, int64_t> key_leases_;
  std::atomic<int64_t> next_id_{1000};
  ExpireCallback on_expire_;
  bool running_ = false;
};

} // namespace etcdmvp
