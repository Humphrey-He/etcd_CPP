#include "etcd_mvp.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <string>

using etcdmvp::DeleteRequest;
using etcdmvp::DeleteResponse;
using etcdmvp::ErrorCode;
using etcdmvp::GetRequest;
using etcdmvp::GetResponse;
using etcdmvp::KV;
using etcdmvp::Lease;
using etcdmvp::LeaseGrantRequest;
using etcdmvp::LeaseGrantResponse;
using etcdmvp::LeaseKeepAliveRequest;
using etcdmvp::LeaseKeepAliveResponse;
using etcdmvp::LeaseRevokeRequest;
using etcdmvp::LeaseRevokeResponse;
using etcdmvp::PutRequest;
using etcdmvp::PutResponse;
using etcdmvp::Watch;
using etcdmvp::WatchCreateRequest;
using etcdmvp::WatchRequest;
using etcdmvp::WatchResponse;

void PrintHeader(const etcdmvp::ResponseHeader& h) {
  std::cout << "code=" << h.code() << " leader=" << h.leader_address() << " msg=" << h.message() << std::endl;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cout << "Usage:\n"
              << "  etcd_mvp_client <addr> put <key> <value> [lease_id]\n"
              << "  etcd_mvp_client <addr> get <key>\n"
              << "  etcd_mvp_client <addr> del <key>\n"
              << "  etcd_mvp_client <addr> watch <prefix> [start_revision]\n"
              << "  etcd_mvp_client <addr> lease-grant <ttl>\n"
              << "  etcd_mvp_client <addr> lease-revoke <id>\n"
              << "  etcd_mvp_client <addr> lease-keepalive <id>\n";
    return 1;
  }

  std::string addr = argv[1];
  std::string cmd = argv[2];

  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  std::unique_ptr<KV::Stub> kv = KV::NewStub(channel);
  std::unique_ptr<Watch::Stub> watch = Watch::NewStub(channel);
  std::unique_ptr<Lease::Stub> lease = Lease::NewStub(channel);

  if (cmd == "put" && argc >= 5) {
    PutRequest req;
    req.set_key(argv[3]);
    req.set_value(argv[4]);
    if (argc >= 6) req.set_lease(std::stoll(argv[5]));
    PutResponse resp;
    grpc::ClientContext ctx;
    auto status = kv->Put(&ctx, req, &resp);
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    PrintHeader(resp.header());
    std::cout << "revision=" << resp.revision() << std::endl;
    return 0;
  }

  if (cmd == "get" && argc >= 4) {
    GetRequest req;
    req.set_key(argv[3]);
    GetResponse resp;
    grpc::ClientContext ctx;
    auto status = kv->Get(&ctx, req, &resp);
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    PrintHeader(resp.header());
    if (resp.kvs_size() > 0) {
      std::cout << "value=" << resp.kvs(0).value() << " revision=" << resp.revision() << std::endl;
    } else {
      std::cout << "(not found) revision=" << resp.revision() << std::endl;
    }
    return 0;
  }

  if (cmd == "del" && argc >= 4) {
    DeleteRequest req;
    req.set_key(argv[3]);
    DeleteResponse resp;
    grpc::ClientContext ctx;
    auto status = kv->Delete(&ctx, req, &resp);
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    PrintHeader(resp.header());
    std::cout << "deleted=" << resp.deleted() << " revision=" << resp.revision() << std::endl;
    return 0;
  }

  if (cmd == "watch" && argc >= 4) {
    WatchRequest req;
    auto* create = req.mutable_create_request();
    create->set_key(argv[3]);
    create->set_prefix(true);
    if (argc >= 5) {
      create->set_start_revision(std::stoll(argv[4]));
    }

    grpc::ClientContext ctx;
    std::unique_ptr<grpc::ClientReader<WatchResponse>> reader(watch->Watch(&ctx, req));
    WatchResponse resp;
    while (reader->Read(&resp)) {
      PrintHeader(resp.header());
      for (const auto& ev : resp.events()) {
        std::string type = ev.type() == etcdmvp::Event::PUT ? "PUT" : "DELETE";
        std::cout << "event=" << type << " key=" << ev.kv().key() << " val=" << ev.kv().value()
                  << " rev=" << resp.revision() << std::endl;
      }
    }
    auto status = reader->Finish();
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    return 0;
  }

  if (cmd == "lease-grant" && argc >= 4) {
    LeaseGrantRequest req;
    req.set_ttl(std::stoll(argv[3]));
    LeaseGrantResponse resp;
    grpc::ClientContext ctx;
    auto status = lease->LeaseGrant(&ctx, req, &resp);
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    PrintHeader(resp.header());
    std::cout << "lease_id=" << resp.id() << " ttl=" << resp.ttl() << std::endl;
    return 0;
  }

  if (cmd == "lease-revoke" && argc >= 4) {
    LeaseRevokeRequest req;
    req.set_id(std::stoll(argv[3]));
    LeaseRevokeResponse resp;
    grpc::ClientContext ctx;
    auto status = lease->LeaseRevoke(&ctx, req, &resp);
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    PrintHeader(resp.header());
    return 0;
  }

  if (cmd == "lease-keepalive" && argc >= 4) {
    LeaseKeepAliveRequest req;
    req.set_id(std::stoll(argv[3]));
    LeaseKeepAliveResponse resp;
    grpc::ClientContext ctx;
    auto status = lease->LeaseKeepAlive(&ctx, req, &resp);
    if (!status.ok()) {
      std::cout << "RPC error: " << status.error_message() << std::endl;
      return 1;
    }
    PrintHeader(resp.header());
    std::cout << "lease_id=" << resp.id() << " ttl=" << resp.ttl() << std::endl;
    return 0;
  }

  std::cout << "Unknown command" << std::endl;
  return 1;
}
