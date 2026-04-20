#pragma once

#include "etcdmvp/raft/raft.h"

#include <vector>

namespace etcdmvp {

struct Message {
  enum class Type { AppendEntries, RequestVote };

  Type type;
  int64_t to;
  int64_t from;
  uint64_t term;

  RaftAppendEntriesRequest append_entries;
  RaftRequestVoteRequest request_vote;
};

struct Ready {
  HardState hard_state;
  bool has_hard_state = false;

  std::vector<LogEntry> entries;

  std::vector<Message> messages;

  std::vector<LogEntry> committed_entries;

  uint64_t snapshot_index = 0;
  std::vector<uint8_t> snapshot_data;
};

class RaftCore {
public:
  enum class Role { Follower, Candidate, Leader };

  RaftCore(int64_t id, const std::vector<int64_t>& peers);

  void LoadState(const std::vector<LogEntry>& log, const HardState& hs);

  Ready GetReady();
  void Advance(const Ready& rd);

  void Tick();

  void Step(const Message& msg);

  bool Propose(const std::string& command, uint64_t* out_index = nullptr);

  bool IsLeader() const;
  int64_t Id() const;
  int64_t LeaderId() const;
  Role GetRole() const;
  uint64_t CommitIndex() const;
  uint64_t LastLogIndex() const;
  uint64_t LastLogTerm() const;
  uint64_t LogBaseIndex() const;

private:
  void BecomeFollower(uint64_t term);
  void BecomeCandidate();
  void BecomeLeader();
  void SendHeartbeats();
  void AdvanceCommitIndex();

  bool LogMatches(uint64_t index, uint64_t term) const;
  size_t Offset(uint64_t index) const;
  uint64_t LogTermAt(uint64_t index) const;

  void HandleAppendEntries(const RaftAppendEntriesRequest& req, int64_t from);
  void HandleRequestVote(const RaftRequestVoteRequest& req, int64_t from);

  void Send(Message msg);

  int64_t id_;
  std::vector<int64_t> peers_;

  Role role_ = Role::Follower;
  HardState hs_;
  std::vector<LogEntry> log_;
  uint64_t log_base_index_ = 0;
  uint64_t last_applied_ = 0;

  int64_t leader_id_ = -1;

  std::unordered_map<int64_t, uint64_t> next_index_;
  std::unordered_map<int64_t, uint64_t> match_index_;

  uint64_t election_timeout_ticks_ = 10;
  uint64_t heartbeat_interval_ticks_ = 1;
  uint64_t elapsed_ticks_ = 0;

  std::vector<Message> msgs_;
  std::vector<LogEntry> unstable_entries_;
  uint64_t committed_index_applied_ = 0;

  bool has_ready_ = false;
};

} // namespace etcdmvp
