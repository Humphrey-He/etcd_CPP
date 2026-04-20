#include <gtest/gtest.h>
#include "etcd_mvp.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace etcdmvp;

TEST(LeaderFailoverTest, KillLeaderAndElectNew) {
  auto ch1 = grpc::CreateChannel("127.0.0.1:2379", grpc::InsecureChannelCredentials());
  auto cluster1 = Cluster::NewStub(ch1);

  grpc::ClientContext status_ctx;
  Empty req;
  StatusResponse status_resp;
  auto status = cluster1->Status(&status_ctx, req, &status_resp);
  EXPECT_TRUE(status.ok());
  int64_t old_leader = status_resp.leader();

  std::string kill_cmd = "pkill -f \"ETCD_MVP_NODE_ID=" + std::to_string(old_leader) + "\"";
  std::system(kill_cmd.c_str());
  std::this_thread::sleep_for(std::chrono::seconds(5));

  int64_t survivor_port = (old_leader == 1) ? 2380 : 2379;
  std::string survivor_addr = "127.0.0.1:" + std::to_string(survivor_port);
  auto ch_survivor = grpc::CreateChannel(survivor_addr, grpc::InsecureChannelCredentials());
  auto cluster_survivor = Cluster::NewStub(ch_survivor);
  auto kv_survivor = KV::NewStub(ch_survivor);

  grpc::ClientContext status_ctx2;
  StatusResponse status_resp2;
  status = cluster_survivor->Status(&status_ctx2, req, &status_resp2);
  EXPECT_TRUE(status.ok());
  int64_t new_leader = status_resp2.leader();
  EXPECT_NE(new_leader, old_leader);

  grpc::ClientContext put_ctx;
  PutRequest put_req;
  put_req.set_key("failover_test");
  put_req.set_value("failover_value");
  PutResponse put_resp;
  status = kv_survivor->Put(&put_ctx, put_req, &put_resp);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(put_resp.header().code(), OK);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
