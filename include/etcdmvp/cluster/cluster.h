#pragma once

#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/raft/raft.h"
#include "etcdmvp/storage/config.h"
#include "etcdmvp/storage/snapshot.h"
#include "etcdmvp/storage/wal.h"
#include "etcdmvp/watch/watch_manager.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace etcdmvp {

class InProcTransport : public ITransport {
public:
  void RegisterNode(int64_t id, RaftNode* node);
  RaftAppendEntriesResponse SendAppendEntries(int64_t target_id, const RaftAppendEntriesRequest& req) override;
  RaftRequestVoteResponse SendRequestVote(int64_t target_id, const RaftRequestVoteRequest& req) override;

private:
  std::unordered_map<int64_t, RaftNode*> nodes_;
};

class MvpCluster {
public:
  explicit MvpCluster(int size);

  void Start();
  void Stop();

  int64_t LeaderId() const;
  RaftNode* Leader() const;
  RaftNode* NodeById(int64_t id) const;

  bool Put(const std::string& key, const std::string& value);
  bool Get(const std::string& key, std::string& value) const;
  bool Delete(const std::string& key);

  uint64_t WatchPrefix(const std::string& prefix, WatchManager::Callback cb, uint64_t start_revision = 0);

  KvEngine* KvById(int64_t id) const;
  WatchManager* WatchById(int64_t id) const;

private:
  void SetupSnapshotHooks();

  int size_;
  std::vector<int64_t> ids_;

  std::unique_ptr<InProcTransport> transport_;
  std::vector<StorageConfig> configs_;
  std::vector<std::unique_ptr<SnapshotFile>> snapshots_;
  std::vector<uint64_t> last_snapshot_revision_;

  std::vector<std::unique_ptr<WatchManager>> watch_managers_;
  std::vector<std::unique_ptr<KvEngine>> kv_engines_;
  std::vector<std::unique_ptr<WalFile>> wals_;
  std::vector<std::unique_ptr<RaftNode>> nodes_;
};

} // namespace etcdmvp
