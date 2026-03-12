#pragma once

#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/lease/lease_manager.h"
#include "etcdmvp/raft/raft.h"
#include "etcdmvp/storage/config.h"
#include "etcdmvp/storage/snapshot.h"
#include "etcdmvp/storage/wal.h"
#include "etcdmvp/watch/watch_manager.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace etcdmvp {

class EtcdNode {
public:
  EtcdNode(int64_t id, const std::unordered_map<int64_t, std::string>& peers);

  void Start();
  void Stop();

  int64_t Id() const;
  int64_t LeaderId() const;
  std::string LeaderAddress() const;

  RaftNode* Raft();
  KvEngine* Kv();
  WatchManager* Watch();
  LeaseManager* Leases();

  const StorageConfig& Config() const;
  std::string LastRecoveryErrorCode() const;
  std::string LastRecoveryErrorMessage() const;

private:
  void LoadSnapshotAndWal();

  int64_t id_;
  std::unordered_map<int64_t, std::string> peers_;
  StorageConfig config_;

  std::unique_ptr<WatchManager> watch_;
  std::unique_ptr<LeaseManager> leases_;
  std::unique_ptr<KvEngine> kv_;
  std::unique_ptr<SnapshotFile> snapshot_;
  std::unique_ptr<WalFile> wal_;
  std::unique_ptr<ITransport> transport_;
  std::unique_ptr<RaftNode> raft_;

  uint64_t last_snapshot_revision_ = 0;

  std::string recovery_error_code_;
  std::string recovery_error_message_;
};

} // namespace etcdmvp
