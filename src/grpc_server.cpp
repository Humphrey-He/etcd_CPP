#include "etcdmvp/grpc_server.h"

#include "etcdmvp/cluster/cluster.h"
#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/watch/watch_manager.h"

#include "etcd_mvp.grpc.pb.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>

namespace etcdmvp {

namespace {
std::atomic<uint64_t> g_req_seq{1};

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
     << "\"message\":\"" << message << "\"";
  ss << "}";
  std::cout << ss.str() << std::endl;
}

std::string LeaderAddress(const std::unordered_map<int64_t, std::string>& peers, int64_t leader_id) {
  auto it = peers.find(leader_id);
  if (it == peers.end()) return "";
  return it->second;
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

class GrpcKvService final : public KV::Service {
public:
  GrpcKvService(int64_t node_id,
                MvpCluster* cluster,
                const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), cluster_(cluster), peer_addresses_(peer_addresses) {}

  grpc::Status Put(grpc::ServerContext* context, const PutRequest* request, PutResponse* response) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "put_received", request_id, "put request received");

    int64_t leader_id = cluster_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      LeaderAddress(peer_addresses_, leader_id),
                                                      request_id,
                                                      "not leader"));
      LogJson("WARN", "put_not_leader", request_id, "request routed to follower");
      return grpc::Status::OK;
    }

    KvEngine* kv = cluster_->KvById(node_id_);
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    uint64_t before = kv->Revision();
    std::string key(reinterpret_cast<const char*>(request->key().data()), request->key().size());
    std::string value(reinterpret_cast<const char*>(request->value().data()), request->value().size());

    LogJson("INFO", "wal_append", request_id, "propose entry");
    RaftNode* leader = cluster_->NodeById(node_id_);
    if (!leader || !leader->Propose("PUT " + key + " " + value)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "propose failed"));
      return grpc::Status::OK;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(RpcTimeoutSeconds());
    while (std::chrono::steady_clock::now() < deadline) {
      if (context->IsCancelled()) break;
      uint64_t now = kv->Revision();
      if (now > before) {
        response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
        response->set_revision(static_cast<int64_t>(now));
        LogJson("INFO", "put_committed", request_id, "commit applied");
        return grpc::Status::OK;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
    return grpc::Status::OK;
  }

  grpc::Status Get(grpc::ServerContext* context, const GetRequest* request, GetResponse* response) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "get_received", request_id, "get request received");

    int64_t leader_id = cluster_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      LeaderAddress(peer_addresses_, leader_id),
                                                      request_id,
                                                      "not leader"));
      return grpc::Status::OK;
    }

    KvEngine* kv = cluster_->KvById(node_id_);
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    std::string key(reinterpret_cast<const char*>(request->key().data()), request->key().size());
    std::string value;
    if (kv->Get(key, value)) {
      auto* kvmsg = response->add_kvs();
      kvmsg->set_key(request->key());
      kvmsg->set_value(value);
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

    int64_t leader_id = cluster_->LeaderId();
    if (leader_id != node_id_) {
      response->mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                      LeaderAddress(peer_addresses_, leader_id),
                                                      request_id,
                                                      "not leader"));
      return grpc::Status::OK;
    }

    KvEngine* kv = cluster_->KvById(node_id_);
    if (!kv) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "kv engine missing"));
      return grpc::Status::OK;
    }

    uint64_t before = kv->Revision();
    std::string key(reinterpret_cast<const char*>(request->key().data()), request->key().size());

    RaftNode* leader = cluster_->NodeById(node_id_);
    if (!leader || !leader->Propose("DEL " + key)) {
      response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "propose failed"));
      return grpc::Status::OK;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(RpcTimeoutSeconds());
    while (std::chrono::steady_clock::now() < deadline) {
      if (context->IsCancelled()) break;
      uint64_t now = kv->Revision();
      if (now > before) {
        response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
        response->set_revision(static_cast<int64_t>(now));
        response->set_deleted(1);
        LogJson("INFO", "delete_committed", request_id, "commit applied");
        return grpc::Status::OK;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    response->mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "timeout"));
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  MvpCluster* cluster_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

class GrpcWatchService final : public Watch::Service {
public:
  GrpcWatchService(int64_t node_id,
                   MvpCluster* cluster,
                   const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), cluster_(cluster), peer_addresses_(peer_addresses) {}

  grpc::Status Watch(grpc::ServerContext* context,
                     const WatchRequest* request,
                     grpc::ServerWriter<WatchResponse>* writer) override {
    std::string request_id = request->request_id().empty() ? GenerateRequestId() : request->request_id();
    LogJson("INFO", "watch_received", request_id, "watch request received");

    int64_t leader_id = cluster_->LeaderId();
    if (leader_id != node_id_) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(NOT_LEADER,
                                                 LeaderAddress(peer_addresses_, leader_id),
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

    KvEngine* kv = cluster_->KvById(node_id_);
    WatchManager* wm = cluster_->WatchById(node_id_);
    if (!kv || !wm) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(INTERNAL, "", request_id, "watch manager missing"));
      writer->Write(resp);
      return grpc::Status::OK;
    }

    int64_t start_rev = request->create_request().start_revision();
    uint64_t current_rev = kv->Revision();
    uint64_t available_start = current_rev + 1;
    if (start_rev > 0 && static_cast<uint64_t>(start_rev) < available_start) {
      WatchResponse resp;
      resp.mutable_header()->CopyFrom(MakeHeader(HISTORY_UNAVAILABLE, "", request_id, "history unavailable"));
      resp.set_compact_revision(static_cast<int64_t>(available_start));
      writer->Write(resp);
      return grpc::Status::OK;
    }

    std::mutex mu;
    std::condition_variable cv;
    std::queue<KvEvent> queue;

    WatchRequest req;
    req.key = request->create_request().key();
    req.prefix = request->create_request().prefix();
    req.start_revision = start_rev > 0 ? static_cast<uint64_t>(start_rev) : available_start;

    uint64_t watch_id = wm->Register(req, [&](const KvEvent& ev) {
      std::lock_guard<std::mutex> lock(mu);
      queue.push(ev);
      cv.notify_one();
    });

    LogJson("INFO", "watch_registered", request_id, "watch registered");

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
      auto* kv = event->mutable_kv();
      kv->set_key(ev.key);
      kv->set_value(ev.value);

      if (!writer->Write(resp)) break;
      LogJson("INFO", "watch_dispatch", request_id, "watch event dispatched");
    }

    wm->Unregister(watch_id);
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  MvpCluster* cluster_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

class GrpcClusterService final : public ::etcdmvp::Cluster::Service {
public:
  GrpcClusterService(int64_t node_id,
                     MvpCluster* cluster,
                     const std::unordered_map<int64_t, std::string>& peer_addresses)
      : node_id_(node_id), cluster_(cluster), peer_addresses_(peer_addresses) {}

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
    response->set_leader(cluster_->LeaderId());
    KvEngine* kv = cluster_->KvById(node_id_);
    if (kv) response->set_revision(static_cast<int64_t>(kv->Revision()));
    response->mutable_header()->CopyFrom(MakeHeader(OK, "", request_id, "ok"));
    return grpc::Status::OK;
  }

private:
  int64_t node_id_;
  MvpCluster* cluster_;
  std::unordered_map<int64_t, std::string> peer_addresses_;
};

} // namespace

GrpcServer::GrpcServer(int64_t node_id,
                       const std::string& address,
                       MvpCluster* cluster,
                       const std::unordered_map<int64_t, std::string>& peer_addresses)
    : node_id_(node_id), address_(address), cluster_(cluster), peer_addresses_(peer_addresses) {}

void GrpcServer::Start() {
  auto kv_service = std::make_unique<GrpcKvService>(node_id_, cluster_, peer_addresses_);
  auto watch_service = std::make_unique<GrpcWatchService>(node_id_, cluster_, peer_addresses_);
  auto cluster_service = std::make_unique<GrpcClusterService>(node_id_, cluster_, peer_addresses_);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
  builder.RegisterService(kv_service.get());
  builder.RegisterService(watch_service.get());
  builder.RegisterService(cluster_service.get());
  server_ = builder.BuildAndStart();

  LogJson("INFO", "grpc_started", GenerateRequestId(), "listening on " + address_);

  server_->Wait();
}

void GrpcServer::Stop() {
  if (server_) server_->Shutdown();
}

} // namespace etcdmvp
