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
  if (wal_) {
    std::vector<LogEntry> loaded;
    HardState hs;
    if (wal_->Load(loaded, hs)) {
      hs_ = hs;
      log_ = std::move(loaded);
    }
  }
  if (log_.empty()) {
    log_.push_back(LogEntry{0, 0, ""});
  }
  log_base_index_ = log_.front().index;
  last_applied_ = log_base_index_;
  election_timeout_ms_ = RandomInRange(300, 500);
}

void RaftNode::SetTransport(ITransport* transport) {
  std::lock_guard<std::mutex> lock(mu_);
  transport_ = transport;
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

int64_t RaftNode::LeaderId() const {
  std::lock_guard<std::mutex> lock(mu_);
  return leader_id_;
}

RaftNode::Role RaftNode::GetRole() const {
  std::lock_guard<std::mutex> lock(mu_);
  return role_;
}

// Handle AppendEntries RPC from leader (heartbeat or log replication)
RaftAppendEntriesResponse RaftNode::OnAppendEntries(const RaftAppendEntriesRequest& req) {
  std::lock_guard<std::mutex> lock(mu_);
  RaftAppendEntriesResponse resp;
  resp.term = hs_.current_term;

  // Reject if sender's term is stale
  if (req.term < hs_.current_term) {
    resp.success = false;
    return resp;
  }

  // Step down if sender has higher term
  if (req.term > hs_.current_term) {
    BecomeFollower(req.term);
  }

  leader_id_ = req.leader_id;
  elapsed_since_heartbeat_ms_ = 0;

  // Reject if log doesn't match at prev_log_index
  if (!LogMatches(req.prev_log_index, req.prev_log_term)) {
    resp.success = false;
    return resp;
  }

  uint64_t index = req.prev_log_index + 1;
  for (const auto& entry : req.entries) {
    if (index <= LastLogIndex()) {
      size_t off = Offset(index);
      if (off < log_.size() && log_[off].term != entry.term) {
        log_.resize(off);
      }
    }
    if (index > LastLogIndex()) {
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

// Handle RequestVote RPC from candidate during election
RaftRequestVoteResponse RaftNode::OnRequestVote(const RaftRequestVoteRequest& req) {
  std::lock_guard<std::mutex> lock(mu_);
  RaftRequestVoteResponse resp;
  resp.term = hs_.current_term;

  // Reject if candidate's term is stale
  if (req.term < hs_.current_term) {
    resp.vote_granted = false;
    return resp;
  }

  // Step down if candidate has higher term
  if (req.term > hs_.current_term) {
    BecomeFollower(req.term);
  }

  // Check if candidate's log is at least as up-to-date as ours
  bool up_to_date = (req.last_log_term > LastLogTerm()) ||
                    (req.last_log_term == LastLogTerm() && req.last_log_index >= LastLogIndex());

  // Grant vote if we haven't voted yet (or already voted for this candidate) and log is up-to-date
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
  return Propose(command, nullptr);
}

// Propose a new command to the Raft log (leader only)
bool RaftNode::Propose(const std::string& command, uint64_t* out_index) {
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
  if (out_index) *out_index = entry.index;
  return true;
}

uint64_t RaftNode::CommitIndex() const {
  std::lock_guard<std::mutex> lock(mu_);
  return hs_.commit_index;
}

uint64_t RaftNode::LastLogIndex() const {
  return log_base_index_ + static_cast<uint64_t>(log_.size() - 1);
}

uint64_t RaftNode::LastLogTerm() const {
  if (log_.empty()) return 0;
  return log_.back().term;
}

uint64_t RaftNode::LogTermAt(uint64_t index) const {
  if (index < log_base_index_) return 0;
  size_t off = index - log_base_index_;
  if (off >= log_.size()) return 0;
  return log_[off].term;
}

uint64_t RaftNode::LogBaseIndex() const { return log_base_index_; }

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
    if (!transport_) continue;
    RaftRequestVoteRequest req;
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
  leader_id_ = id_;
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
  uint64_t base_index;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (role_ != Role::Leader) return;
    term = hs_.current_term;
    commit_index = hs_.commit_index;
    base_index = log_base_index_;
  }

  for (auto peer : peers) {
    if (!transport_) continue;
    RaftAppendEntriesRequest req;
    {
      std::lock_guard<std::mutex> lock(mu_);
      uint64_t next = next_index_[peer];
      if (next < base_index + 1) next = base_index + 1;
      req.term = term;
      req.leader_id = id_;
      req.prev_log_index = next - 1;
      req.prev_log_term = LogTermAt(req.prev_log_index);
      req.leader_commit = commit_index;
      if (next <= LastLogIndex()) {
        size_t off = Offset(next);
        req.entries.assign(log_.begin() + static_cast<long>(off), log_.end());
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
      if (next_index_[peer] > base_index + 1) next_index_[peer] -= 1;
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
    if (match > static_cast<int>(peers_.size() + 1) / 2 && LogTermAt(idx) == hs_.current_term) {
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
    if (last_applied_ < log_base_index_) last_applied_ = log_base_index_;
    while (last_applied_ < hs_.commit_index) {
      last_applied_++;
      if (last_applied_ >= log_base_index_ && last_applied_ <= LastLogIndex()) {
        size_t off = Offset(last_applied_);
        if (off < log_.size()) {
          to_apply.push_back(log_[off]);
        }
      }
    }
  }
  for (const auto& entry : to_apply) {
    if (state_machine_) state_machine_->Apply(entry);
  }
}

bool RaftNode::LogMatches(uint64_t index, uint64_t term) const {
  return LogTermAt(index) == term;
}

size_t RaftNode::Offset(uint64_t index) const {
  return static_cast<size_t>(index - log_base_index_);
}

} // namespace etcdmvp
