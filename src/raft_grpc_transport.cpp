#include "etcdmvp/raft/raft_grpc_transport.h"

#include "etcd_mvp.grpc.pb.h"

#include <chrono>

namespace etcdmvp {

GrpcRaftTransport::GrpcRaftTransport(const std::unordered_map<int64_t, std::string>& peers) : peers_(peers) {}

RaftAppendEntriesResponse GrpcRaftTransport::SendAppendEntries(int64_t target_id, const RaftAppendEntriesRequest& req) {
  RaftAppendEntriesResponse resp;
  auto& stub = StubFor(target_id);
  if (!stub) return resp;

  ::etcdmvp::AppendEntriesRequest pb;
  pb.set_term(req.term);
  pb.set_leader_id(req.leader_id);
  pb.set_prev_log_index(req.prev_log_index);
  pb.set_prev_log_term(req.prev_log_term);
  pb.set_leader_commit(req.leader_commit);
  for (const auto& e : req.entries) {
    auto* entry = pb.add_entries();
    entry->set_term(e.term);
    entry->set_index(e.index);
    entry->set_data(e.command);
  }

  ::etcdmvp::AppendEntriesResponse pb_resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  auto status = stub->AppendEntries(&ctx, pb, &pb_resp);
  if (!status.ok()) return resp;

  resp.term = pb_resp.term();
  resp.success = pb_resp.success();
  resp.match_index = pb_resp.match_index();
  return resp;
}

RaftRequestVoteResponse GrpcRaftTransport::SendRequestVote(int64_t target_id, const RaftRequestVoteRequest& req) {
  RaftRequestVoteResponse resp;
  auto& stub = StubFor(target_id);
  if (!stub) return resp;

  ::etcdmvp::RequestVoteRequest pb;
  pb.set_term(req.term);
  pb.set_candidate_id(req.candidate_id);
  pb.set_last_log_index(req.last_log_index);
  pb.set_last_log_term(req.last_log_term);

  ::etcdmvp::RequestVoteResponse pb_resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  auto status = stub->RequestVote(&ctx, pb, &pb_resp);
  if (!status.ok()) return resp;

  resp.term = pb_resp.term();
  resp.vote_granted = pb_resp.vote_granted();
  return resp;
}

std::unique_ptr<::etcdmvp::Raft::Stub>& GrpcRaftTransport::StubFor(int64_t target_id) {
  auto it = stubs_.find(target_id);
  if (it != stubs_.end()) return it->second;
  auto addr_it = peers_.find(target_id);
  if (addr_it == peers_.end()) {
    stubs_[target_id] = nullptr;
    return stubs_[target_id];
  }
  auto channel = grpc::CreateChannel(addr_it->second, grpc::InsecureChannelCredentials());
  stubs_[target_id] = ::etcdmvp::Raft::NewStub(channel);
  return stubs_[target_id];
}

} // namespace etcdmvp
