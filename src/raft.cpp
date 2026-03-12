#include "etcdmvp/raft/raft.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace etcdmvp {

namespace {
uint64_t RandomInRange(uint64_t min_ms, uint64_t max_ms) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> dist(min_ms, max_ms);
  return dist(rng);
}
}

RaftNode::RaftNode(int64_t id,
                   const std::vector<int64_t>& peers,
                   ITransport* transport,
                   IWal* wal,
                   IStateMachine* state_machine)
    : id_(id),
      peers_(peers),
      transport_(transport),
      wal_(wal),
      state_machine_(state_machine) {
  log_.push_back(LogEntry{});
  if (wal_) {
    std::vector<LogEntry> loaded;
    HardState hs;
    if (wal_->Load(loaded, hs)) {
      hs_ = hs;
      log_ = std::move(loaded);
      if (log_.empty()) {
        log_.push_back(LogEntry{});
      }
    }
  }
  election_timeout_ms_ = RandomInRange(300, 500);
}

void RaftNode::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return;
  running_ = true;
  std::thread([this]() { RunLoop(); }).detach();
}

void RaftNode::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  running_ = false;
}

bool RaftNode::IsLeader() const {
  std::lock_guard<std::mutex> lock(mu_);
  return role_ == Role::Leader;
}

int64_t RaftNode::Id() const { return id_; }

RaftNode::Role RaftNode::GetRole() const {
  std::lock_guard<std::mutex> lock(mu_);
  return role_;
}

AppendEntriesResponse RaftNode::OnAppendEntries(const AppendEntriesRequest& req) {
  std::lock_guard<std::mutex> lock(mu_);
  AppendEntriesResponse resp;
  resp.term = hs_.current_term;

  if (req.term < hs_.current_term) {
    resp.success = false;
    return resp;
  }

  if (req.term > hs_.current_term) {
    BecomeFollower(req.term);
  }

  elapsed_since_heartbeat_ms_ = 0;

  if (!LogMatches(req.prev_log_index, req.prev_log_term)) {
    resp.success = false;
    return resp;
  }

  uint64_t index = req.prev_log_index + 1;
  for (const auto& entry : req.entries) {
    if (index < log_.size()) {
      if (log_[index].term != entry.term) {
        log_.resize(index);
      }
    }
    if (index >= log_.size()) {
      log_.push_back(entry);
      if (wal_) {
        wal_->Append(entry);
      }
    }
    index++;
  }

  if (req.leader_commit > hs_.commit_index) {
    hs_.commit_index = std::min(req.leader_commit, LastLogIndex());
    if (wal_) {
      wal_->SaveHardState(hs_);
    }
  }

  resp.term = hs_.current_term;
  resp.success = true;
  resp.match_index = LastLogIndex();
  return resp;
}

RequestVoteResponse RaftNode::OnRequestVote(const RequestVoteRequest& req) {
  std::lock_guard<std::mutex> lock(mu_);
  RequestVoteResponse resp;
  resp.term = hs_.current_term;

  if (req.term < hs_.current_term) {
    resp.vote_granted = false;
    return resp;
  }

  if (req.term > hs_.current_term) {
    BecomeFollower(req.term);
  }

  bool up_to_date = (req.last_log_term > LastLogTerm()) ||
                    (req.last_log_term == LastLogTerm() && req.last_log_index >= LastLogIndex());

  if ((hs_.voted_for == -1 || hs_.voted_for == req.candidate_id) && up_to_date) {
    hs_.voted_for = req.candidate_id;
    if (wal_) {
      wal_->SaveHardState(hs_);
    }
    resp.vote_granted = true;
  } else {
    resp.vote_granted = false;
  }

  resp.term = hs_.current_term;
  return resp;
}

bool RaftNode::Propose(const std::string& command) {
  std::lock_guard<std::mutex> lock(mu_);
  if (role_ != Role::Leader) return false;
  LogEntry entry;
  entry.index = LastLogIndex() + 1;
  entry.term = hs_.current_term;
  entry.command = command;
  log_.push_back(entry);
  if (wal_) {
    wal_->Append(entry);
  }
  return true;
}

void RaftNode::RunLoop() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!running_) break;
    }
    Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void RaftNode::Tick() {
  Role role;
  {
    std::lock_guard<std::mutex> lock(mu_);
    elapsed_since_heartbeat_ms_ += 10;
    role = role_;
    if (role_ == Role::Leader && elapsed_since_heartbeat_ms_ >= heartbeat_interval_ms_) {
      elapsed_since_heartbeat_ms_ = 0;
    }
  }

  if (role == Role::Leader) {
    SendHeartbeats();
    AdvanceCommitIndex();
    ApplyCommitted();
    return;
  }

  bool election = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (elapsed_since_heartbeat_ms_ >= election_timeout_ms_) {
      election = true;
      elapsed_since_heartbeat_ms_ = 0;
    }
  }
  if (election) {
    BecomeCandidate();
  }

  ApplyCommitted();
}

void RaftNode::BecomeFollower(uint64_t term) {
  role_ = Role::Follower;
  hs_.current_term = term;
  hs_.voted_for = -1;
  elapsed_since_heartbeat_ms_ = 0;
  election_timeout_ms_ = RandomInRange(300, 500);
  if (wal_) {
    wal_->SaveHardState(hs_);
  }
}

void RaftNode::BecomeCandidate() {
  uint64_t term;
  uint64_t last_index;
  uint64_t last_term;
  {
    std::lock_guard<std::mutex> lock(mu_);
    role_ = Role::Candidate;
    hs_.current_term += 1;
    hs_.voted_for = id_;
    term = hs_.current_term;
    last_index = LastLogIndex();
    last_term = LastLogTerm();
    if (wal_) {
      wal_->SaveHardState(hs_);
    }
  }

  int votes = 1;
  for (auto peer : peers_) {
    RequestVoteRequest req;
    req.term = term;
    req.candidate_id = id_;
    req.last_log_index = last_index;
    req.last_log_term = last_term;
    auto resp = transport_->SendRequestVote(peer, req);
    if (resp.term > term) {
      std::lock_guard<std::mutex> lock(mu_);
      BecomeFollower(resp.term);
      return;
    }
    if (resp.vote_granted) votes++;
  }

  if (votes > static_cast<int>(peers_.size() + 1) / 2) {
    BecomeLeader();
  }
}

void RaftNode::BecomeLeader() {
  std::lock_guard<std::mutex> lock(mu_);
  role_ = Role::Leader;
  for (auto peer : peers_) {
    next_index_[peer] = LastLogIndex() + 1;
    match_index_[peer] = 0;
  }
  elapsed_since_heartbeat_ms_ = 0;
}

void RaftNode::SendHeartbeats() {
  std::vector<int64_t> peers = peers_;
  uint64_t term;
  uint64_t commit_index;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::Leader) return;
    term = hs_.current_term;
    commit_index = hs_.commit_index;
  }

  for (auto peer : peers) {
    AppendEntriesRequest req;
    {
      std::lock_guard<std::mutex> lock(mu_);
      uint64_t next = next_index_[peer];
      req.term = term;
      req.leader_id = id_;
      req.prev_log_index = next - 1;
      req.prev_log_term = (next - 1 < log_.size()) ? log_[next - 1].term : 0;
      req.leader_commit = commit_index;
      if (next < log_.size()) {
        req.entries.assign(log_.begin() + static_cast<long>(next), log_.end());
      }
    }

    auto resp = transport_->SendAppendEntries(peer, req);
    std::lock_guard<std::mutex> lock(mu_);
    if (resp.term > hs_.current_term) {
      BecomeFollower(resp.term);
      return;
    }
    if (resp.success) {
      match_index_[peer] = req.prev_log_index + req.entries.size();
      next_index_[peer] = match_index_[peer] + 1;
    } else {
      if (next_index_[peer] > 1) next_index_[peer] -= 1;
    }
  }
}

void RaftNode::AdvanceCommitIndex() {
  std::lock_guard<std::mutex> lock(mu_);
  if (role_ != Role::Leader) return;
  for (uint64_t idx = LastLogIndex(); idx > hs_.commit_index; --idx) {
    int match = 1;
    for (auto peer : peers_) {
      if (match_index_[peer] >= idx) match++;
    }
    if (match > static_cast<int>(peers_.size() + 1) / 2 && log_[idx].term == hs_.current_term) {
      hs_.commit_index = idx;
      if (wal_) {
        wal_->SaveHardState(hs_);
      }
      break;
    }
  }
}

void RaftNode::ApplyCommitted() {
  std::vector<LogEntry> to_apply;
  {
    std::lock_guard<std::mutex> lock(mu_);
    while (last_applied_ < hs_.commit_index) {
      last_applied_++;
      if (last_applied_ < log_.size()) {
        to_apply.push_back(log_[last_applied_]);
      }
    }
  }
  for (const auto& entry : to_apply) {
    if (state_machine_) state_machine_->Apply(entry);
  }
}

uint64_t RaftNode::LastLogIndex() const {
  return static_cast<uint64_t>(log_.size() - 1);
}

uint64_t RaftNode::LastLogTerm() const {
  return log_.empty() ? 0 : log_.back().term;
}

bool RaftNode::LogMatches(uint64_t index, uint64_t term) const {
  if (index >= log_.size()) return false;
  return log_[index].term == term;
}

} // namespace etcdmvp
