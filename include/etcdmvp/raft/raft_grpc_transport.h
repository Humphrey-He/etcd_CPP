#pragma once

#include "etcdmvp/raft/raft.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace etcdmvp {

class GrpcRaftTransport : public ITransport {
public:
  explicit GrpcRaftTransport(const std::unordered_map<int64_t, std::string>& peers);

  RaftAppendEntriesResponse SendAppendEntries(int64_t target_id, const RaftAppendEntriesRequest& req) override;
  RaftRequestVoteResponse SendRequestVote(int64_t target_id, const RaftRequestVoteRequest& req) override;

private:
  std::unique_ptr<::etcdmvp::Raft::Stub>& StubFor(int64_t target_id);

  std::unordered_map<int64_t, std::string> peers_;
  std::unordered_map<int64_t, std::unique_ptr<::etcdmvp::Raft::Stub>> stubs_;
};

} // namespace etcdmvp
