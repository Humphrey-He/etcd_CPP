#pragma once

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
  explicit KvEngine(WatchManager* watch_manager);

  void Apply(const LogEntry& entry) override;

  bool Put(const std::string& key, const std::string& value, uint64_t* out_revision);
  bool Delete(const std::string& key, uint64_t* out_revision);
  bool Get(const std::string& key, std::string& value) const;

  uint64_t Revision() const;

  void Snapshot(std::unordered_map<std::string, std::string>& out_kv, uint64_t& out_revision) const;
  void LoadSnapshot(const std::unordered_map<std::string, std::string>& kv, uint64_t revision);

private:
  void EmitEvent(const KvEvent& ev);

  WatchManager* watch_manager_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::string> kv_;
  uint64_t revision_ = 0;
};

} // namespace etcdmvp
