#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace etcdmvp {

struct LogEntry {
  uint64_t index = 0;
  uint64_t term = 0;
  std::string command;
};

struct HardState {
  uint64_t current_term = 0;
  int64_t voted_for = -1;
  uint64_t commit_index = 0;
};

struct AppendEntriesRequest {
  uint64_t term = 0;
  int64_t leader_id = -1;
  uint64_t prev_log_index = 0;
  uint64_t prev_log_term = 0;
  std::vector<LogEntry> entries;
  uint64_t leader_commit = 0;
};

struct AppendEntriesResponse {
  uint64_t term = 0;
  bool success = false;
  uint64_t match_index = 0;
};

struct RequestVoteRequest {
  uint64_t term = 0;
  int64_t candidate_id = -1;
  uint64_t last_log_index = 0;
  uint64_t last_log_term = 0;
};

struct RequestVoteResponse {
  uint64_t term = 0;
  bool vote_granted = false;
};

class ITransport {
public:
  virtual ~ITransport() = default;
  virtual AppendEntriesResponse SendAppendEntries(int64_t target_id, const AppendEntriesRequest& req) = 0;
  virtual RequestVoteResponse SendRequestVote(int64_t target_id, const RequestVoteRequest& req) = 0;
};

class IStateMachine {
public:
  virtual ~IStateMachine() = default;
  virtual void Apply(const LogEntry& entry) = 0;
};

class IWal {
public:
  virtual ~IWal() = default;
  virtual bool Append(const LogEntry& entry) = 0;
  virtual bool SaveHardState(const HardState& hs) = 0;
  virtual bool Load(std::vector<LogEntry>& out_log, HardState& out_hs) = 0;
};

class RaftNode {
public:
  enum class Role { Follower, Candidate, Leader };

  RaftNode(int64_t id,
           const std::vector<int64_t>& peers,
           ITransport* transport,
           IWal* wal,
           IStateMachine* state_machine);

  void Start();
  void Stop();

  bool IsLeader() const;
  int64_t Id() const;
  Role GetRole() const;

  AppendEntriesResponse OnAppendEntries(const AppendEntriesRequest& req);
  RequestVoteResponse OnRequestVote(const RequestVoteRequest& req);

  bool Propose(const std::string& command);

private:
  void RunLoop();
  void Tick();
  void BecomeFollower(uint64_t term);
  void BecomeCandidate();
  void BecomeLeader();
  void SendHeartbeats();
  void AdvanceCommitIndex();
  void ApplyCommitted();

  uint64_t LastLogIndex() const;
  uint64_t LastLogTerm() const;
  bool LogMatches(uint64_t index, uint64_t term) const;

  int64_t id_;
  std::vector<int64_t> peers_;
  ITransport* transport_;
  IWal* wal_;
  IStateMachine* state_machine_;

  mutable std::mutex mu_;
  Role role_ = Role::Follower;
  bool running_ = false;

  HardState hs_;
  std::vector<LogEntry> log_; // log_[0] is dummy
  uint64_t last_applied_ = 0;

  std::unordered_map<int64_t, uint64_t> next_index_;
  std::unordered_map<int64_t, uint64_t> match_index_;

  uint64_t election_timeout_ms_ = 400;
  uint64_t heartbeat_interval_ms_ = 100;
  uint64_t elapsed_since_heartbeat_ms_ = 0;
};

} // namespace etcdmvp
