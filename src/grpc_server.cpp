#include "etcdmvp/grpc_server.h"

#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/watch/watch_manager.h"

#include "etcd_mvp.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace etcdmvp {

namespace {
std::atomic<uint64_t> g_req_seq{1};

struct TxnOp {
  enum class Type { Put = 1, Del = 2, Get = 3 };
  Type type;
  std::string key;
  std::string value;
  int64_t lease = 0;
};

std::string GenerateRequestId() {
  uint64_t v = g_req_seq.fetch_add(1);
  std::ostringstream ss;
  ss << "req-" << v;
  return ss.str();
}

int RpcTimeoutSeconds() {
  const char* env = std::getenv("ETCD_MVP_RPC_TIMEOUT_S");
  if (!env) return 5;
  int v = std::atoi(env);
  return v > 0 ? v : 5;
}

void LogJson(const std::string& level,
             const std::string& event,
             const std::string& request_id,
             const std::string& message) {
  auto now = std::chrono::system_clock::now();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  std::ostringstream ss;
  ss << "{"
     << "\"ts\":" << secs << ","
     << "\"level\":\"" << level << "\","
     << "\"event\":\"" << event << "\","
     << "\"request_id\":\"" << request_id << "\","
     << "\"trace_id\":\"" << request_id << "\","
     << "\"message\":\"" << message << "\"";
  ss << "}";
  std::cout << ss.str() << std::endl;
}

ResponseHeader MakeHeader(ErrorCode code,
                          const std::string& leader_address,
                          const std::string& request_id,
                          const std::string& message) {
  ResponseHeader h;
  h.set_code(code);
  h.set_leader_address(leader_address);
  h.set_request_id(request_id);
  h.set_message(message);
  return h;
}

bool WaitForRevision(KvEngine* kv, uint64_t before, int timeout_seconds, grpc::ServerContext* ctx, uint64_t& out_rev) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx->IsCancelled()) break;
    uint64_t now = kv->Revision();
    if (now > before) {
      out_rev = now;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForCommit(RaftNode* raft, uint64_t index, int timeout_seconds, grpc::ServerContext* ctx) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx->IsCancelled()) break;
    if (raft->CommitIndex() >= index) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void WriteU8(std::string& out, uint8_t v) {
  out.push_back(static_cast<char>(v));
}

void WriteU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

void WriteU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

std::string EncodeTxn(const std::vector<TxnOp>& ops) {
  std::string out;
  out.append("TXN1", 4);
  WriteU32(out, static_cast<uint32_t>(ops.size()));
  for (const auto& op : ops) {
    WriteU8(out, static_cast<uint8_t>(op.type));
    WriteU32(out, static_cast<uint32_t>(op.key.size()));
    out.append(op.key);
    WriteU32(out, static_cast<uint32_t>(op.value.size()));
    out.append(op.value);
    WriteU64(out, static_cast<uint64_t>(op.lease));
  }
  return out;
}

class GrpcKvService final : public KV::Service {
public:
  GrpcKvService(int64_t node_id,
                EtcdNode* node,
                const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), node_(node), peer_addresses_(peer_addresses) {}

  grpc::Status Put(grpc::ServerContext* context, const PutRequest* request, PutResponse* response) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "put_received", request_id, "put request received");

    int64_t leader_id = node_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      node_->LeaderAddress(),
                                                      request_id,
                                                      "not leader"));
      LogJson("WARN", "put_not_leader", request_id, "request routed to follower");
      return grpc::Status::OK;
    }

    KvEngine* kv = node_->Kv();
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    uint64_t before = kv->Revision();
    std::string key(reinterpret_cast<const char*>(request->key().data()), request->key().size());
    std::string value(reinterpret_cast<const char*>(request->value().data()), request->value().size());

    LogJson("INFO", "wal_append", request_id, "propose entry");
    RaftNode* leader = node_->Raft();
    std::ostringstream cmd;
    cmd << "PUT " << key << " " << request->lease() << " " << value;
    if (!leader || !leader->Propose(cmd.str())) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "propose failed"));
      return grpc::Status::OK;
    }

    uint64_t committed = 0;
    if (WaitForRevision(kv, before, RpcTimeoutSeconds(), context, committed)) {
      response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
      response->set_revision(static_cast<int64_t>(committed));
      LogJson("INFO", "put_committed", request_id, "commit applied");
      return grpc::Status::OK;
    }

    response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
    return grpc::Status::OK;
  }

  grpc::Status Get(grpc::ServerContext* context, const GetRequest* request, GetResponse* response) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "get_received", request_id, "get request received");

    int64_t leader_id = node_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      node_->LeaderAddress(),
                                                      request_id,
                                                      "not leader"));
      return grpc::Status::OK;
    }

    KvEngine* kv = node_->Kv();
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    std::string key(reinterpret_cast<const char*>(request->key().data()), request->key().size());
    std::string value;
    uint64_t create_rev = 0;
    uint64_t mod_rev = 0;
    uint64_t version = 0;
    int64_t lease = 0;

    if (kv->Get(key, value)) {
      kv->GetMetadata(key, create_rev, mod_rev, version, lease);
      auto* kvmsg = response->add_kvs();
      kvmsg->set_key(request->key());
      kvmsg->set_value(value);
      kvmsg->set_create_revision(static_cast<int64_t>(create_rev));
      kvmsg->set_mod_revision(static_cast<int64_t>(mod_rev));
      kvmsg->set_version(static_cast<int64_t>(version));
      kvmsg->set_lease(lease);
      response->set_revision(static_cast<int64_t>(kv->Revision()));
      response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    } else {
      response->set_revision(static_cast<int64_t>(kv->Revision()));
      response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "not found"));
    }
    return grpc::Status::OK;
  }

  grpc::Status Delete(grpc::ServerContext* context, const DeleteRequest* request, DeleteResponse* response) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "delete_received", request_id, "delete request received");

    int64_t leader_id = node_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      node_->LeaderAddress(),
                                                      request_id,
                                                      "not leader"));
      return grpc::Status::OK;
    }

    KvEngine* kv = node_->Kv();
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    uint64_t before = kv->Revision();
    std::string key(reinterpret_cast<const char*>(request->key().data()), request->key().size());

    RaftNode* leader = node_->Raft();
    if (!leader || !leader->Propose("DEL " + key)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "propose failed"));
      return grpc::Status::OK;
    }

    uint64_t committed = 0;
    if (WaitForRevision(kv, before, RpcTimeoutSeconds(), context, committed)) {
      response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
      response->set_revision(static_cast<int64_t>(committed));
      response->set_deleted(1);
      LogJson("INFO", "delete_committed", request_id, "commit applied");
      return grpc::Status::OK;
    }

    response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
    return grpc::Status::OK;
  }

  grpc::Status Txn(grpc::ServerContext* context, const TxnRequest* request, TxnResponse* response) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "txn_received", request_id, "txn request received");

    int64_t leader_id = node_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      node_->LeaderAddress(),
                                                      request_id,
                                                      "not leader"));
      return grpc::Status::OK;
    }

    KvEngine* kv = node_->Kv();
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    bool succeeded = true;
    for (const auto& cmp : request->compare()) {
      std::string key(reinterpret_cast<const char*>(cmp.key().data()), cmp.key().size());
      std::string value;
      uint64_t create_rev = 0;
      uint64_t mod_rev = 0;
      uint64_t version = 0;
      int64_t lease = 0;
      bool exists = kv->Get(key, value);
      kv->GetMetadata(key, create_rev, mod_rev, version, lease);

      bool ok = false;
      if (cmp.target() == VALUE) {
        std::string cmp_val(reinterpret_cast<const char*>(cmp.value().data()), cmp.value().size());
        if (!exists) value.clear();
        if (cmp.result() == EQUAL) ok = (value == cmp_val);
        if (cmp.result() == NOT_EQUAL) ok = (value != cmp_val);
      } else if (cmp.target() == MOD) {
        int64_t rev = static_cast<int64_t>(mod_rev);
        if (cmp.result() == EQUAL) ok = (rev == cmp.revision());
        if (cmp.result() == NOT_EQUAL) ok = (rev != cmp.revision());
        if (cmp.result() == GREATER) ok = (rev > cmp.revision());
        if (cmp.result() == LESS) ok = (rev < cmp.revision());
      }
      if (!ok) {
        succeeded = false;
        break;
      }
    }

    const auto& ops = succeeded ? request->success() : request->failure();
    std::vector<TxnOp> write_ops;
    for (const auto& op : ops) {
      if (op.has_put_request()) {
        TxnOp t;
        t.type = TxnOp::Type::Put;
        t.key = op.put_request().key();
        t.value = op.put_request().value();
        t.lease = op.put_request().lease();
        write_ops.push_back(std::move(t));
      } else if (op.has_delete_request()) {
        TxnOp t;
        t.type = TxnOp::Type::Del;
        t.key = op.delete_request().key();
        write_ops.push_back(std::move(t));
      }
    }

    std::string entry = EncodeTxn(write_ops);
    uint64_t index = 0;
    if (!node_->Raft()->Propose(entry, &index)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "propose failed"));
      return grpc::Status::OK;
    }

    if (!WaitForCommit(node_->Raft(), index, RpcTimeoutSeconds(), context)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
      return grpc::Status::OK;
    }

    response->set_succeeded(succeeded);

    for (const auto& op : ops) {
      ResponseOp* resp = response->add_responses();
      if (op.has_put_request()) {
        PutResponse put_resp;
        put_resp.mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
        put_resp.set_revision(static_cast<int64_t>(kv->Revision()));
        resp->mutable_put_response()->CopyFrom(put_resp);
      } else if (op.has_get_request()) {
        GetResponse get_resp;
        GetRequest req = op.get_request();
        Get(context, &req, &get_resp);
        resp->mutable_get_response()->CopyFrom(get_resp);
      } else if (op.has_delete_request()) {
        DeleteResponse del_resp;
        del_resp.mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
        del_resp.set_revision(static_cast<int64_t>(kv->Revision()));
        del_resp.set_deleted(1);
        resp->mutable_delete_response()->CopyFrom(del_resp);
      }
    }

    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  EtcdNode* node_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

class GrpcWatchService final : public Watch::Service {
public:
  GrpcWatchService(int64_t node_id,
                   EtcdNode* node,
                   const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), node_(node), peer_addresses_(peer_addresses) {}

  grpc::Status Watch(grpc::ServerContext* context,
                     const WatchRequest* request,
                     grpc::ServerWriter<WatchResponse>* writer) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "watch_received", request_id, "watch request received");

    int64_t leader_id = node_->LeaderId();
    if (leader_id != node_id_) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                 node_->LeaderAddress(),
                                                 request_id,
                                                 "not leader"));
      writer->Write(resp);
      return grpc::Status::OK;
    }

    if (!request->has_create_request()) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "create request required"));
      writer->Write(resp);
      return grpc::Status::OK;
    }

    WatchManager* wm = node_->Watch();
    if (!wm) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "watch manager missing"));
      writer->Write(resp);
      return grpc::Status::OK;
    }

    int64_t start_rev = request->create_request().start_revision();
    uint64_t oldest = wm->OldestRevision();
    if (start_rev > 0 && static_cast<uint64_t>(start_rev) < oldest) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(HISTORY_UNAVAILABLE, "", request_id, "history unavailable"));
      resp.set_compact_revision(static_cast<int64_t>(oldest));
      writer->Write(resp);
      return grpc::Status::OK;
    }

    std::vector<KvEvent> history;
    uint64_t start = start_rev > 0 ? static_cast<uint64_t>(start_rev) : oldest;
    wm->GetHistory(start, history);

    std::mutex mu;
    std::condition_variable cv;
    std::queue<KvEvent> queue;

    WatchRequest req;
    req.key = request->create_request().key();
    req.prefix = request->create_request().prefix();
    req.start_revision = start;

    uint64_t watch_id = wm->Register(req, [&](const KvEvent& ev) {
      std::lock_guard<std::mutex> lock(mu);
      queue.push(ev);
      cv.notify_one();
    });

    LogJson("INFO", "watch_registered", request_id, "watch registered");

    for (const auto& ev : history) {
      if (!request->create_request().prefix() && ev.key != request->create_request().key()) continue;
      if (request->create_request().prefix() && ev.key.rfind(request->create_request().key(), 0) != 0) continue;
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
      resp.set_watch_id(static_cast<int64_t>(watch_id));
      resp.set_revision(static_cast<int64_t>(ev.revision));
      auto* event = resp.add_events();
      event->set_type(ev.type == KvEvent::Type::Put ? Event::PUT : Event::DELETE);
      auto* kvmsg = event->mutable_kv();
      kvmsg->set_key(ev.key);
      kvmsg->set_value(ev.value);
      if (!writer->Write(resp)) break;
    }

    while (!context->IsCancelled()) {
      KvEvent ev;
      {
        std::unique_lock<std::mutex> lock(mu);
        if (queue.empty()) {
          cv.wait_for(lock, std::chrono::milliseconds(200));
          if (queue.empty()) continue;
        }
        ev = queue.front();
        queue.pop();
      }

      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
      resp.set_watch_id(static_cast<int64_t>(watch_id));
      resp.set_revision(static_cast<int64_t>(ev.revision));
      auto* event = resp.add_events();
      event->set_type(ev.type == KvEvent::Type::Put ? Event::PUT : Event::DELETE);
      auto* kvmsg = event->mutable_kv();
      kvmsg->set_key(ev.key);
      kvmsg->set_value(ev.value);

      if (!writer->Write(resp)) break;
      LogJson("INFO", "watch_dispatch", request_id, "watch event dispatched");
    }

    wm->Unregister(watch_id);
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  EtcdNode* node_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

class GrpcLeaseService final : public Lease::Service {
public:
  GrpcLeaseService(int64_t node_id,
                   EtcdNode* node,
                   const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), node_(node), peer_addresses_(peer_addresses) {}

  grpc::Status LeaseGrant(grpc::ServerContext* context,
                          const LeaseGrantRequest* request,
                          LeaseGrantResponse* response) override {
    std::string request_id = GenerateRequestId();
    if (node_->LeaderId() != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER, node_->LeaderAddress(), request_id, "not leader"));
      return grpc::Status::OK;
    }
    int64_t ttl = request->ttl();
    if (ttl <= 0) ttl = 5;
    int64_t id = node_->Leases()->Grant(ttl);

    uint64_t index = 0;
    std::ostringstream cmd;
    cmd << "LEASE_GRANT " << id << " " << ttl;
    if (!node_->Raft()->Propose(cmd.str(), &index) ||
        !WaitForCommit(node_->Raft(), index, RpcTimeoutSeconds(), context)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
      return grpc::Status::OK;
    }

    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    response->set_id(id);
    response->set_ttl(ttl);
    return grpc::Status::OK;
  }

  grpc::Status LeaseRevoke(grpc::ServerContext* context,
                           const LeaseRevokeRequest* request,
                           LeaseRevokeResponse* response) override {
    std::string request_id = GenerateRequestId();
    if (node_->LeaderId() != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER, node_->LeaderAddress(), request_id, "not leader"));
      return grpc::Status::OK;
    }
    uint64_t index = 0;
    std::ostringstream cmd;
    cmd << "LEASE_REVOKE " << request->id();
    if (!node_->Raft()->Propose(cmd.str(), &index) ||
        !WaitForCommit(node_->Raft(), index, RpcTimeoutSeconds(), context)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
      return grpc::Status::OK;
    }
    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    return grpc::Status::OK;
  }

  grpc::Status LeaseKeepAlive(grpc::ServerContext* context,
                              const LeaseKeepAliveRequest* request,
                              LeaseKeepAliveResponse* response) override {
    std::string request_id = GenerateRequestId();
    if (node_->LeaderId() != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER, node_->LeaderAddress(), request_id, "not leader"));
      return grpc::Status::OK;
    }
    int64_t keep_ttl = 0;
    if (!node_->Leases()->GetTTL(request->id(), keep_ttl)) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_FOUND, "", request_id, "lease not found"));
      return grpc::Status::OK;
    }
    uint64_t index = 0;
    std::ostringstream cmd;
    cmd << "LEASE_KEEPALIVE " << request->id() << " " << keep_ttl;
    if (!node_->Raft()->Propose(cmd.str(), &index) ||
        !WaitForCommit(node_->Raft(), index, RpcTimeoutSeconds(), context)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
      return grpc::Status::OK;
    }
    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    response->set_id(request->id());
    response->set_ttl(keep_ttl);
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  EtcdNode* node_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

class GrpcClusterService final : public ::etcdmvp::Cluster::Service {
public:
  GrpcClusterService(int64_t node_id,
                     EtcdNode* node,
                     const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), node_(node), peer_addresses_(peer_addresses) {}

  grpc::Status MemberList(grpc::ServerContext*, const Empty*, MemberListResponse* response) override {
    std::string request_id = GenerateRequestId();
    for (const auto& kv : peer_addresses_) {
      auto* m = response->add_members();
      m->set_id(kv.first);
      m->set_name("node" + std::to_string(kv.first));
      m->add_peer_urls(kv.second);
    }
    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    return grpc::Status::OK;
  }

  grpc::Status Status(grpc::ServerContext*, const Empty*, StatusResponse* response) override {
    std::string request_id = GenerateRequestId();
    response->set_leader(node_->LeaderId());
    KvEngine* kv = node_->Kv();
    if (kv) response->set_revision(static_cast<int64_t>(kv->Revision()));
    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  EtcdNode* node_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

class GrpcRaftService final : public ::etcdmvp::Raft::Service {
public:
  explicit GrpcRaftService(EtcdNode* node) : node_(node) {}

  grpc::Status AppendEntries(grpc::ServerContext*, const ::etcdmvp::AppendEntriesRequest* request,
                             ::etcdmvp::AppendEntriesResponse* response) override {
    RaftAppendEntriesRequest req;
    req.term = request->term();
    req.leader_id = request->leader_id();
    req.prev_log_index = request->prev_log_index();
    req.prev_log_term = request->prev_log_term();
    req.leader_commit = request->leader_commit();
    for (const auto& entry : request->entries()) {
      req.entries.push_back(LogEntry{entry.index(), entry.term(), entry.data()});
    }
    auto resp = node_->Raft()->OnAppendEntries(req);
    response->set_term(resp.term);
    response->set_success(resp.success);
    response->set_match_index(resp.match_index);
    return grpc::Status::OK;
  }

  grpc::Status RequestVote(grpc::ServerContext*, const ::etcdmvp::RequestVoteRequest* request,
                           ::etcdmvp::RequestVoteResponse* response) override {
    RaftRequestVoteRequest req;
    req.term = request->term();
    req.candidate_id = request->candidate_id();
    req.last_log_index = request->last_log_index();
    req.last_log_term = request->last_log_term();
    auto resp = node_->Raft()->OnRequestVote(req);
    response->set_term(resp.term);
    response->set_vote_granted(resp.vote_granted);
    return grpc::Status::OK;
  }

private:
  EtcdNode* node_;
};

} // namespace

GrpcServer::GrpcServer(int64_t node_id,
                       const std::string& address,
                       EtcdNode* node,
                       const std::unordered_map<int64_t, std::string>& peer_addresses)
    : node_id_(node_id), address_(address), node_(node), peer_addresses_(peer_addresses) {}

void GrpcServer::Start() {
  auto kv_service = std::make_unique<GrpcKvService>(node_id_, node_, peer_addresses_);
  auto watch_service = std::make_unique<GrpcWatchService>(node_id_, node_, peer_addresses_);
  auto lease_service = std::make_unique<GrpcLeaseService>(node_id_, node_, peer_addresses_);
  auto cluster_service = std::make_unique<GrpcClusterService>(node_id_, node_, peer_addresses_);
  auto raft_service = std::make_unique<GrpcRaftService>(node_);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
  builder.RegisterService(kv_service.get());
  builder.RegisterService(watch_service.get());
  builder.RegisterService(lease_service.get());
  builder.RegisterService(cluster_service.get());
  builder.RegisterService(raft_service.get());
  server_ = builder.BuildAndStart();

  LogJson("INFO", "grpc_started", GenerateRequestId(), "listening on " + address_);

  server_->Wait();
}

void GrpcServer::Stop() {
  if (server_) server_->Shutdown();
}

} // namespace etcdmvp



