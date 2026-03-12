#include "etcdmvp/cluster/cluster.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace etcdmvp {

void InProcTransport::RegisterNode(int64_t id, RaftNode* node) {
  nodes_[id] = node;
}

AppendEntriesResponse InProcTransport::SendAppendEntries(int64_t target_id, const AppendEntriesRequest& req) {
  auto it = nodes_.find(target_id);
  if (it == nodes_.end()) return AppendEntriesResponse{};
  return it->second->OnAppendEntries(req);
}

RequestVoteResponse InProcTransport::SendRequestVote(int64_t target_id, const RequestVoteRequest& req) {
  auto it = nodes_.find(target_id);
  if (it == nodes_.end()) return RequestVoteResponse{};
  return it->second->OnRequestVote(req);
}

MvpCluster::MvpCluster(int size) : size_(size) {
  transport_ = std::make_unique<InProcTransport>();
  std::filesystem::create_directories("data");
  for (int i = 0; i < size_; ++i) {
    ids_.push_back(i + 1);
  }

  for (int i = 0; i < size_; ++i) {
    watch_managers_.push_back(std::make_unique<WatchManager>());
    kv_engines_.push_back(std::make_unique<KvEngine>(watch_managers_.back().get()));
    std::string prefix = "data/node" + std::to_string(ids_[i]);
    wals_.push_back(std::make_unique<WalFile>(prefix));
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
  if (!leader->Propose("PUT " + key + " " + value)) return false;
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
