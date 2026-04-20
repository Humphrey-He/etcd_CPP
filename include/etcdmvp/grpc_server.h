#pragma once

#include "etcdmvp/node.h"
#include "etcdmvp/tls/tls_config.h"
#include "etcdmvp/auth/auth_manager.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace etcdmvp {

class GrpcServer {
public:
  GrpcServer(int64_t node_id,
             const std::string& address,
             EtcdNode* node,
             const std::unordered_map<int64_t, std::string>& peer_addresses);

  void Start();
  void Stop();

  AuthManager* Auth() { return &auth_; }

private:
  int64_t node_id_;
  std::string address_;
  EtcdNode* node_;
  std::unordered_map<int64_t, std::string> peer_addresses_;

  TlsConfig tls_;
  AuthManager auth_;
  std::unique_ptr<grpc::Server> server_;
};

} // namespace etcdmvp
