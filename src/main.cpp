#include "etcdmvp/grpc_server.h"
#include "etcdmvp/cluster/cluster.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace etcdmvp;

int main() {
  MvpCluster cluster(3);
  cluster.Start();

  std::unordered_map<int64_t, std::string> addresses;
  addresses[1] = "0.0.0.0:2379";
  addresses[2] = "0.0.0.0:2380";
  addresses[3] = "0.0.0.0:2381";

  std::vector<std::thread> threads;
  std::vector<std::unique_ptr<GrpcServer>> servers;
  servers.push_back(std::make_unique<GrpcServer>(1, addresses[1], &cluster, addresses));
  servers.push_back(std::make_unique<GrpcServer>(2, addresses[2], &cluster, addresses));
  servers.push_back(std::make_unique<GrpcServer>(3, addresses[3], &cluster, addresses));

  for (size_t i = 0; i < servers.size(); ++i) {
    threads.emplace_back([&servers, i]() { servers[i]->Start(); });
  }

  std::cout << "etcd MVP gRPC servers started on 2379/2380/2381." << std::endl;
  std::cout << "Type 'exit' to stop." << std::endl;

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit" || line == "quit") break;
  }

  for (auto& s : servers) s->Stop();
  cluster.Stop();

  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }
  return 0;
}
