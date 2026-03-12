#include "etcdmvp/grpc_server.h"
#include "etcdmvp/node.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

using namespace etcdmvp;

namespace {
std::unordered_map<int64_t, std::string> ParsePeers(const std::string& peers) {
  std::unordered_map<int64_t, std::string> map;
  std::stringstream ss(peers);
  std::string item;
  while (std::getline(ss, item, ',')) {
    auto pos = item.find('=');
    if (pos == std::string::npos) continue;
    int64_t id = std::stoll(item.substr(0, pos));
    std::string addr = item.substr(pos + 1);
    map[id] = addr;
  }
  return map;
}
}

int main(int argc, char** argv) {
  int64_t node_id = 1;
  std::string listen = "0.0.0.0:2379";
  std::string peers_env;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--id" && i + 1 < argc) {
      node_id = std::stoll(argv[++i]);
    } else if (arg == "--listen" && i + 1 < argc) {
      listen = argv[++i];
    } else if (arg == "--peers" && i + 1 < argc) {
      peers_env = argv[++i];
    }
  }

  if (peers_env.empty()) {
    const char* env = std::getenv("ETCD_MVP_PEERS");
    if (env) peers_env = env;
  }

  if (const char* env = std::getenv("ETCD_MVP_NODE_ID")) {
    node_id = std::stoll(env);
  }

  if (const char* env = std::getenv("ETCD_MVP_LISTEN")) {
    listen = env;
  }

  auto peers = ParsePeers(peers_env);
  if (peers.empty()) {
    peers[1] = "127.0.0.1:2379";
    peers[2] = "127.0.0.1:2380";
    peers[3] = "127.0.0.1:2381";
  }

  EtcdNode node(node_id, peers);
  node.Start();

  GrpcServer server(node_id, listen, &node, peers);
  std::thread t([&server]() { server.Start(); });

  std::cout << "Node " << node_id << " listening on " << listen << std::endl;
  std::cout << "Type 'exit' to stop." << std::endl;

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit" || line == "quit") break;
  }

  server.Stop();
  node.Stop();
  if (t.joinable()) t.join();
  return 0;
}
