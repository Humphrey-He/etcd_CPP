#pragma once

#include "etcdmvp/lease/lease_manager.h"
#include "etcdmvp/raft/raft.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace etcdmvp {

struct KvEvent {
  enum class Type { Put, Delete };
  Type type;
  std::string key;
  std::string value;
  uint64_t revision;
};

class WatchManager;

class KvEngine : public IStateMachine {
public:
  explicit KvEngine(WatchManager* watch_manager, LeaseManager* lease_manager = nullptr);

  void Apply(const LogEntry& entry) override;

  bool Put(const std::string& key, const std::string& value, uint64_t* out_revision);
  bool PutWithLease(const std::string& key,
                    const std::string& value,
                    int64_t lease_id,
                    uint64_t* out_revision);
  bool Delete(const std::string& key, uint64_t* out_revision);
  bool Get(const std::string& key, std::string& value) const;

  uint64_t Revision() const;

  void Snapshot(std::unordered_map<std::string, std::string>& out_kv, uint64_t& out_revision) const;
  void LoadSnapshot(const std::unordered_map<std::string, std::string>& kv, uint64_t revision);

  void SetSnapshotHook(std::function<void(uint64_t)> hook);

  bool GetMetadata(const std::string& key,
                   uint64_t& create_rev,
                   uint64_t& mod_rev,
                   uint64_t& version,
                   int64_t& lease) const;

private:
  struct Version {
    uint64_t revision = 0;
    std::string value;
    bool deleted = false;
    int64_t lease_id = 0;
  };

  void EmitEvent(const KvEvent& ev);

  WatchManager* watch_manager_;
  LeaseManager* lease_manager_;
  std::function<void(uint64_t)> snapshot_hook_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::vector<Version>> history_;
  uint64_t revision_ = 0;
};

} // namespace etcdmvp
