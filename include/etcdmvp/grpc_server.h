#pragma once

#include "etcdmvp/cluster/cluster.h"

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
             MvpCluster* cluster,
             const std::unordered_map<int64_t, std::string>& peer_addresses);

  void Start();
  void Stop();

private:
  int64_t node_id_;
  std::string address_;
  MvpCluster* cluster_;
  std::unordered_map<int64_t, std::string> peer_addresses_;

  std::unique_ptr<grpc::Server> server_;
};

} // namespace etcdmvp
