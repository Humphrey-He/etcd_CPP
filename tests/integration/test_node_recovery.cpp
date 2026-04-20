#include <gtest/gtest.h>
#include "etcd_mvp.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace etcdmvp;

TEST(NodeRecoveryTest, RestartAndRecover) {
  auto ch1 = grpc::CreateChannel("127.0.0.1:2379", grpc::InsecureChannelCredentials());
  auto kv1 = KV::NewStub(ch1);

  grpc::ClientContext put_ctx;
  PutRequest put_req;
  put_req.set_key("recovery_test");
  put_req.set_value("recovery_value");
  PutResponse put_resp;
  auto status = kv1->Put(&put_ctx, put_req, &put_resp);
  EXPECT_TRUE(status.ok());

  std::this_thread::sleep_for(std::chrono::seconds(1));

  std::system("pkill -f \"ETCD_MVP_NODE_ID=3\"");
  std::this_thread::sleep_for(std::chrono::seconds(2));

  std::system("ETCD_MVP_NODE_ID=3 ETCD_MVP_LISTEN=\"0.0.0.0:2381\" "
              "ETCD_MVP_PEERS=\"1=127.0.0.1:2379,2=127.0.0.1:2380,3=127.0.0.1:2381\" "
              "./Release/etcd_mvp &");
  std::this_thread::sleep_for(std::chrono::seconds(3));

  auto ch3 = grpc::CreateChannel("127.0.0.1:2381", grpc::InsecureChannelCredentials());
  auto kv3 = KV::NewStub(ch3);

  grpc::ClientContext get_ctx;
  GetRequest get_req;
  get_req.set_key("recovery_test");
  GetResponse get_resp;
  status = kv3->Get(&get_ctx, get_req, &get_resp);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(get_resp.kvs_size(), 1);
  EXPECT_EQ(get_resp.kvs(0).value(), "recovery_value");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
