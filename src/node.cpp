#include "etcdmvp/node.h"
#include "etcdmvp/raft/raft_grpc_transport.h"

#include <filesystem>

namespace etcdmvp {

EtcdNode::EtcdNode(int64_t id, const std::unordered_map<int64_t, std::string>& peers)
    : id_(id), peers_(peers), config_(LoadStorageConfig(id)) {
  std::filesystem::create_directories(config_.data_dir);
  watch_ = std::make_unique<WatchManager>(config_.watch_history_size);

  leases_ = std::make_unique<LeaseManager>([this](const std::string& key) {
    if (!raft_ || !raft_->IsLeader()) return;
    raft_->Propose("DEL " + key);
  });

  kv_ = std::make_unique<KvEngine>(watch_.get(), leases_.get());
  snapshot_ = std::make_unique<SnapshotFile>(config_);
  wal_ = std::make_unique<WalFile>(config_);

  LoadSnapshotAndWal();

  std::vector<int64_t> peer_ids;
  for (const auto& kv : peers_) {
    if (kv.first != id_) peer_ids.push_back(kv.first);
  }

  auto transport = std::make_unique<GrpcRaftTransport>(peers_);
  raft_ = std::make_unique<RaftNode>(id_, peer_ids, transport.get(), wal_.get(), kv_.get());
  raft_->SetTransport(transport.get());
  transport_ = std::move(transport);

  kv_->SetSnapshotHook([this](uint64_t revision) {
    if (config_.snapshot_threshold_entries == 0) return;
    if (revision < last_snapshot_revision_ + config_.snapshot_threshold_entries) return;

    std::unordered_map<std::string, std::string> kv;
    uint64_t rev = 0;
    kv_->Snapshot(kv, rev);

    uint64_t term = 0;
    if (raft_) term = raft_->LogTermAt(rev);

    std::string meta = "{\"node_id\":" + std::to_string(id_) + ",\"revision\":" +
                       std::to_string(rev) + "}";

    if (snapshot_->Save(kv, rev, term, meta)) {
      last_snapshot_revision_ = rev;
      wal_->PurgeUpTo(rev);
      watch_->SaveHistory(config_.data_dir + "/watch_history.bin");
    }
  });
}

void EtcdNode::LoadSnapshotAndWal() {
  std::unordered_map<std::string, std::string> kv;
  uint64_t snap_rev = 0;
  uint64_t snap_term = 0;
  std::string meta;
  if (snapshot_->Load(kv, snap_rev, snap_term, meta)) {
    kv_->LoadSnapshot(kv, snap_rev);
    last_snapshot_revision_ = snap_rev;
  } else {
    if (snapshot_->LastErrorCode() != "SNAPSHOT_NOT_FOUND") {
      recovery_error_code_ = snapshot_->LastErrorCode();
      recovery_error_message_ = snapshot_->LastErrorMessage();
    }
  }

  watch_->LoadHistory(config_.data_dir + "/watch_history.bin");

  wal_->SetSnapshotBase(snap_rev, snap_term);

  std::vector<LogEntry> log;
  HardState hs;
  if (!wal_->Load(log, hs)) {
    recovery_error_code_ = wal_->LastErrorCode();
    recovery_error_message_ = wal_->LastErrorMessage();
  }
}

void EtcdNode::Start() {
  leases_->Start();
  if (raft_) raft_->Start();
}

void EtcdNode::Stop() {
  if (raft_) raft_->Stop();
  if (leases_) leases_->Stop();
}

int64_t EtcdNode::Id() const { return id_; }

int64_t EtcdNode::LeaderId() const {
  if (!raft_) return -1;
  return raft_->LeaderId();
}

std::string EtcdNode::LeaderAddress() const {
  auto it = peers_.find(LeaderId());
  if (it == peers_.end()) return "";
  return it->second;
}

RaftNode* EtcdNode::Raft() { return raft_.get(); }
KvEngine* EtcdNode::Kv() { return kv_.get(); }
WatchManager* EtcdNode::Watch() { return watch_.get(); }
LeaseManager* EtcdNode::Leases() { return leases_.get(); }

const StorageConfig& EtcdNode::Config() const { return config_; }

std::string EtcdNode::LastRecoveryErrorCode() const { return recovery_error_code_; }
std::string EtcdNode::LastRecoveryErrorMessage() const { return recovery_error_message_; }

} // namespace etcdmvp
