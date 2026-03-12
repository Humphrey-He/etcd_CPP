#include "gtest/gtest.h"

#include "etcdmvp/raft/raft.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace etcdmvp {

namespace {
class DummyWal : public IWal {
public:
  bool Append(const LogEntry&) override { return true; }
  bool SaveHardState(const HardState&) override { return true; }
  bool Load(std::vector<LogEntry>& out_log, HardState& out_hs) override {
    out_log.clear();
    out_hs = HardState{};
    return true;
  }
};

class DummyTransport : public ITransport {
public:
  RaftAppendEntriesResponse SendAppendEntries(int64_t, const RaftAppendEntriesRequest&) override {
    RaftAppendEntriesResponse resp;
    resp.term = 1;
    resp.success = true;
    resp.match_index = 0;
    return resp;
  }
  RaftRequestVoteResponse SendRequestVote(int64_t, const RaftRequestVoteRequest&) override {
    RaftRequestVoteResponse resp;
    resp.term = 1;
    resp.vote_granted = true;
    return resp;
  }
};

class DummyStateMachine : public IStateMachine {
public:
  void Apply(const LogEntry& entry) override { last_command = entry.command; }
  std::string last_command;
};
}

TEST(RaftNodeTest, SingleNodeBecomesLeaderAndApplies) {
  DummyWal wal;
  DummyTransport transport;
  DummyStateMachine sm;

  RaftNode node(1, {}, &transport, &wal, &sm);
  node.Start();

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (node.IsLeader()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ASSERT_TRUE(node.IsLeader());
  ASSERT_TRUE(node.Propose("PUT a 0 v"));

  auto apply_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < apply_deadline) {
    if (!sm.last_command.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  node.Stop();
  EXPECT_FALSE(sm.last_command.empty());
}

} // namespace etcdmvp
