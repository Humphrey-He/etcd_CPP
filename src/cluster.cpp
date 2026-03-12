#include "etcdmvp/cluster/cluster.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace etcdmvp {

void InProcTransport::RegisterNode(int64_t id, RaftNode* node) {
  nodes_[id] = node;
}

RaftAppendEntriesResponse InProcTransport::SendAppendEntries(int64_t target_id, const RaftAppendEntriesRequest& req) {
  auto it = nodes_.find(target_id);
  if (it == nodes_.end()) return RaftAppendEntriesResponse{};
  return it->second->OnAppendEntries(req);
}

RaftRequestVoteResponse InProcTransport::SendRequestVote(int64_t target_id, const RaftRequestVoteRequest& req) {
  auto it = nodes_.find(target_id);
  if (it == nodes_.end()) return RaftRequestVoteResponse{};
  return it->second->OnRequestVote(req);
}

MvpCluster::MvpCluster(int size) : size_(size) {
  transport_ = std::make_unique<InProcTransport>();
  for (int i = 0; i < size_; ++i) {
    ids_.push_back(i + 1);
  }

  for (int i = 0; i < size_; ++i) {
    StorageConfig cfg = LoadStorageConfig(ids_[i]);
    configs_.push_back(cfg);
    std::filesystem::create_directories(cfg.data_dir);

    watch_managers_.push_back(std::make_unique<WatchManager>(cfg.watch_history_size));
    kv_engines_.push_back(std::make_unique<KvEngine>(watch_managers_.back().get(), nullptr));

    auto snapshot = std::make_unique<SnapshotFile>(cfg);
    std::unordered_map<std::string, std::string> kv;
    uint64_t snap_rev = 0;
    uint64_t snap_term = 0;
    std::string meta;
    if (snapshot->Load(kv, snap_rev, snap_term, meta)) {
      kv_engines_.back()->LoadSnapshot(kv, snap_rev);
      last_snapshot_revision_.push_back(snap_rev);
    } else {
      last_snapshot_revision_.push_back(0);
    }
    snapshots_.push_back(std::move(snapshot));

    auto wal = std::make_unique<WalFile>(cfg);
    wal->SetSnapshotBase(last_snapshot_revision_.back(), snap_term);
    wals_.push_back(std::move(wal));
  }

  for (int i = 0; i < size_; ++i) {
    std::vector<int64_t> peers;
    for (int j = 0; j < size_; ++j) {
      if (i != j) peers.push_back(ids_[j]);
    }
    nodes_.push_back(std::make_unique<RaftNode>(
        ids_[i], peers, transport_.get(), wals_[i].get(), kv_engines_[i].get()));
  }

  for (int i = 0; i < size_; ++i) {
    transport_->RegisterNode(ids_[i], nodes_[i].get());
  }

  SetupSnapshotHooks();
}

void MvpCluster::SetupSnapshotHooks() {
  for (size_t i = 0; i < kv_engines_.size(); ++i) {
    kv_engines_[i]->SetSnapshotHook([this, i](uint64_t revision) {
      const StorageConfig& cfg = configs_[i];
      if (cfg.snapshot_threshold_entries == 0) return;
      if (revision < last_snapshot_revision_[i] + cfg.snapshot_threshold_entries) return;

      std::unordered_map<std::string, std::string> kv;
      uint64_t rev = 0;
      kv_engines_[i]->Snapshot(kv, rev);

      uint64_t term = 0;
      if (nodes_[i]) {
        term = nodes_[i]->LogTermAt(rev);
      }

      std::string meta = "{\"node_id\":" + std::to_string(ids_[i]) + ",\"revision\":" +
                         std::to_string(rev) + "}";

      if (snapshots_[i]->Save(kv, rev, term, meta)) {
        last_snapshot_revision_[i] = rev;
        wals_[i]->PurgeUpTo(rev);
      }
    });
  }
}

void MvpCluster::Start() {
  for (auto& node : nodes_) node->Start();
}

void MvpCluster::Stop() {
  for (auto& node : nodes_) node->Stop();
}

int64_t MvpCluster::LeaderId() const {
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i]->IsLeader()) return ids_[i];
  }
  return -1;
}

RaftNode* MvpCluster::Leader() const {
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i]->IsLeader()) return nodes_[i].get();
  }
  return nullptr;
}

RaftNode* MvpCluster::NodeById(int64_t id) const {
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (ids_[i] == id) return nodes_[i].get();
  }
  return nullptr;
}

bool MvpCluster::Put(const std::string& key, const std::string& value) {
  RaftNode* leader = Leader();
  if (!leader) return false;
  uint64_t before = kv_engines_[leader->Id() - 1]->Revision();
  if (!leader->Propose("PUT " + key + " 0 " + value)) return false;
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t now = kv_engines_[leader->Id() - 1]->Revision();
    if (now > before) return true;
  }
  return false;
}

bool MvpCluster::Get(const std::string& key, std::string& value) const {
  RaftNode* leader = Leader();
  if (!leader) return false;
  return kv_engines_[leader->Id() - 1]->Get(key, value);
}

bool MvpCluster::Delete(const std::string& key) {
  RaftNode* leader = Leader();
  if (!leader) return false;
  uint64_t before = kv_engines_[leader->Id() - 1]->Revision();
  if (!leader->Propose("DEL " + key)) return false;
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t now = kv_engines_[leader->Id() - 1]->Revision();
    if (now > before) return true;
  }
  return false;
}

uint64_t MvpCluster::WatchPrefix(const std::string& prefix, WatchManager::Callback cb, uint64_t start_revision) {
  RaftNode* leader = Leader();
  if (!leader) return 0;
  WatchRequest req;
  req.key = prefix;
  req.prefix = true;
  req.start_revision = start_revision;
  return watch_managers_[leader->Id() - 1]->Register(req, std::move(cb));
}

KvEngine* MvpCluster::KvById(int64_t id) const {
  if (id <= 0 || static_cast<size_t>(id) > kv_engines_.size()) return nullptr;
  return kv_engines_[id - 1].get();
}

WatchManager* MvpCluster::WatchById(int64_t id) const {
  if (id <= 0 || static_cast<size_t>(id) > watch_managers_.size()) return nullptr;
  return watch_managers_[id - 1].get();
}

} // namespace etcdmvp
