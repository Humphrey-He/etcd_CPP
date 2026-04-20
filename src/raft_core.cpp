#include "etcdmvp/raft/ready.h"

#include <algorithm>
#include <random>

namespace etcdmvp {

namespace {
uint64_t RandomInRange(uint64_t min_ticks, uint64_t max_ticks) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> dist(min_ticks, max_ticks);
  return dist(rng);
}
}

RaftCore::RaftCore(int64_t id, const std::vector<int64_t>& peers)
    : id_(id), peers_(peers) {
  election_timeout_ticks_ = RandomInRange(10, 20);
}

void RaftCore::LoadState(const std::vector<LogEntry>& log, const HardState& hs) {
  hs_ = hs;
  log_ = log;
  if (log_.empty()) {
    log_.push_back(LogEntry{0, 0, ""});
  }
  log_base_index_ = log_.front().index;
  last_applied_ = log_base_index_;
  committed_index_applied_ = hs_.commit_index;
}

Ready RaftCore::GetReady() {
  Ready rd;

  if (hs_.current_term > 0 || hs_.voted_for != -1 || hs_.commit_index > 0) {
    rd.hard_state = hs_;
    rd.has_hard_state = true;
  }

  rd.entries = std::move(unstable_entries_);
  unstable_entries_.clear();

  rd.messages = std::move(msgs_);
  msgs_.clear();

  if (hs_.commit_index > committed_index_applied_) {
    for (uint64_t i = committed_index_applied_ + 1; i <= hs_.commit_index; ++i) {
      if (i > log_base_index_ && i <= LastLogIndex()) {
        rd.committed_entries.push_back(log_[Offset(i)]);
      }
    }
    committed_index_applied_ = hs_.commit_index;
  }

  has_ready_ = false;
  return rd;
}

void RaftCore::Advance(const Ready& rd) {
  if (!rd.entries.empty()) {
    last_applied_ = rd.entries.back().index;
  }
}

void RaftCore::Tick() {
  elapsed_ticks_++;

  if (role_ == Role::Leader) {
    if (elapsed_ticks_ >= heartbeat_interval_ticks_) {
      elapsed_ticks_ = 0;
      SendHeartbeats();
      has_ready_ = true;
    }
  } else {
    if (elapsed_ticks_ >= election_timeout_ticks_) {
      elapsed_ticks_ = 0;
      BecomeCandidate();
      has_ready_ = true;
    }
  }
}

void RaftCore::Step(const Message& msg) {
  if (msg.term > hs_.current_term) {
    BecomeFollower(msg.term);
  }

  switch (msg.type) {
    case Message::Type::AppendEntries:
      HandleAppendEntries(msg.append_entries, msg.from);
      break;
    case Message::Type::RequestVote:
      HandleRequestVote(msg.request_vote, msg.from);
      break;
  }

  has_ready_ = true;
}

bool RaftCore::Propose(const std::string& command, uint64_t* out_index) {
  if (role_ != Role::Leader) return false;

  uint64_t index = LastLogIndex() + 1;
  LogEntry entry{index, hs_.current_term, command};
  log_.push_back(entry);
  unstable_entries_.push_back(entry);

  if (out_index) *out_index = index;

  match_index_[id_] = index;
  SendHeartbeats();
  has_ready_ = true;
  return true;
}

bool RaftCore::IsLeader() const {
  return role_ == Role::Leader;
}

int64_t RaftCore::Id() const {
  return id_;
}

int64_t RaftCore::LeaderId() const {
  return leader_id_;
}

RaftCore::Role RaftCore::GetRole() const {
  return role_;
}

uint64_t RaftCore::CommitIndex() const {
  return hs_.commit_index;
}

uint64_t RaftCore::LastLogIndex() const {
  return log_.empty() ? 0 : log_.back().index;
}

uint64_t RaftCore::LastLogTerm() const {
  return log_.empty() ? 0 : log_.back().term;
}

uint64_t RaftCore::LogBaseIndex() const {
  return log_base_index_;
}

void RaftCore::BecomeFollower(uint64_t term) {
  role_ = Role::Follower;
  hs_.current_term = term;
  hs_.voted_for = -1;
  leader_id_ = -1;
  elapsed_ticks_ = 0;
}

void RaftCore::BecomeCandidate() {
  role_ = Role::Candidate;
  hs_.current_term++;
  hs_.voted_for = id_;
  elapsed_ticks_ = 0;
  election_timeout_ticks_ = RandomInRange(10, 20);

  int votes = 1;
  if (votes > static_cast<int>(peers_.size()) / 2) {
    BecomeLeader();
    return;
  }

  for (int64_t peer : peers_) {
    if (peer == id_) continue;

    Message msg;
    msg.type = Message::Type::RequestVote;
    msg.to = peer;
    msg.from = id_;
    msg.term = hs_.current_term;
    msg.request_vote.term = hs_.current_term;
    msg.request_vote.candidate_id = id_;
    msg.request_vote.last_log_index = LastLogIndex();
    msg.request_vote.last_log_term = LastLogTerm();
    Send(msg);
  }
}

void RaftCore::BecomeLeader() {
  role_ = Role::Leader;
  leader_id_ = id_;
  elapsed_ticks_ = 0;

  next_index_.clear();
  match_index_.clear();
  for (int64_t peer : peers_) {
    next_index_[peer] = LastLogIndex() + 1;
    match_index_[peer] = 0;
  }
  match_index_[id_] = LastLogIndex();

  SendHeartbeats();
}

void RaftCore::SendHeartbeats() {
  for (int64_t peer : peers_) {
    if (peer == id_) continue;

    uint64_t next = next_index_[peer];
    uint64_t prev_index = next - 1;
    uint64_t prev_term = LogTermAt(prev_index);

    Message msg;
    msg.type = Message::Type::AppendEntries;
    msg.to = peer;
    msg.from = id_;
    msg.term = hs_.current_term;
    msg.append_entries.term = hs_.current_term;
    msg.append_entries.leader_id = id_;
    msg.append_entries.prev_log_index = prev_index;
    msg.append_entries.prev_log_term = prev_term;
    msg.append_entries.leader_commit = hs_.commit_index;

    for (uint64_t i = next; i <= LastLogIndex(); ++i) {
      msg.append_entries.entries.push_back(log_[Offset(i)]);
    }

    Send(msg);
  }
}

void RaftCore::AdvanceCommitIndex() {
  if (role_ != Role::Leader) return;

  for (uint64_t n = hs_.commit_index + 1; n <= LastLogIndex(); ++n) {
    if (LogTermAt(n) != hs_.current_term) continue;

    int count = 1;
    for (int64_t peer : peers_) {
      if (peer == id_) continue;
      if (match_index_[peer] >= n) count++;
    }

    if (count > static_cast<int>(peers_.size()) / 2) {
      hs_.commit_index = n;
    }
  }
}

bool RaftCore::LogMatches(uint64_t index, uint64_t term) const {
  if (index < log_base_index_) return false;
  if (index > LastLogIndex()) return false;
  return LogTermAt(index) == term;
}

size_t RaftCore::Offset(uint64_t index) const {
  return static_cast<size_t>(index - log_base_index_);
}

uint64_t RaftCore::LogTermAt(uint64_t index) const {
  if (index < log_base_index_ || index > LastLogIndex()) return 0;
  return log_[Offset(index)].term;
}

void RaftCore::HandleAppendEntries(const RaftAppendEntriesRequest& req, int64_t from) {
  elapsed_ticks_ = 0;
  leader_id_ = from;

  Message resp;
  resp.type = Message::Type::AppendEntries;
  resp.to = from;
  resp.from = id_;
  resp.term = hs_.current_term;

  RaftAppendEntriesResponse r;
  r.term = hs_.current_term;

  if (!LogMatches(req.prev_log_index, req.prev_log_term)) {
    r.success = false;
    resp.append_entries = RaftAppendEntriesRequest();
    Send(resp);
    return;
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
      unstable_entries_.push_back(entry);
    }
    index++;
  }

  if (req.leader_commit > hs_.commit_index) {
    hs_.commit_index = std::min(req.leader_commit, LastLogIndex());
  }

  r.success = true;
  r.match_index = LastLogIndex();
  Send(resp);
}

void RaftCore::HandleRequestVote(const RaftRequestVoteRequest& req, int64_t from) {
  Message resp;
  resp.type = Message::Type::RequestVote;
  resp.to = from;
  resp.from = id_;
  resp.term = hs_.current_term;

  RaftRequestVoteResponse r;
  r.term = hs_.current_term;

  bool up_to_date = (req.last_log_term > LastLogTerm()) ||
                    (req.last_log_term == LastLogTerm() && req.last_log_index >= LastLogIndex());

  if ((hs_.voted_for == -1 || hs_.voted_for == req.candidate_id) && up_to_date) {
    hs_.voted_for = req.candidate_id;
    r.vote_granted = true;
    elapsed_ticks_ = 0;
  } else {
    r.vote_granted = false;
  }

  resp.request_vote = RaftRequestVoteRequest();
  Send(resp);
}

void RaftCore::Send(Message msg) {
  msgs_.push_back(std::move(msg));
}

} // namespace etcdmvp
