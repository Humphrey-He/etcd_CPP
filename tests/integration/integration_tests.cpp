#include <gtest/gtest.h>
#include "etcd_mvp.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>

using namespace etcdmvp;

class IntegrationTest : public ::testing::Test {
protected:
  std::unique_ptr<KV::Stub> kv1_;
  std::unique_ptr<KV::Stub> kv2_;
  std::unique_ptr<KV::Stub> kv3_;
  std::unique_ptr<Cluster::Stub> cluster1_;
  std::unique_ptr<Watch::Stub> watch1_;
  std::unique_ptr<Lease::Stub> lease1_;

  void SetUp() override {
    auto ch1 = grpc::CreateChannel("127.0.0.1:2379", grpc::InsecureChannelCredentials());
    auto ch2 = grpc::CreateChannel("127.0.0.1:2380", grpc::InsecureChannelCredentials());
    auto ch3 = grpc::CreateChannel("127.0.0.1:2381", grpc::InsecureChannelCredentials());

    kv1_ = KV::NewStub(ch1);
    kv2_ = KV::NewStub(ch2);
    kv3_ = KV::NewStub(ch3);
    cluster1_ = Cluster::NewStub(ch1);
    watch1_ = Watch::NewStub(ch1);
    lease1_ = Lease::NewStub(ch1);
  }

  int64_t GetLeader() {
    grpc::ClientContext ctx;
    Empty req;
    StatusResponse resp;
    auto status = cluster1_->Status(&ctx, req, &resp);
    EXPECT_TRUE(status.ok());
    return resp.leader();
  }

  KV::Stub* GetLeaderStub() {
    int64_t leader = GetLeader();
    if (leader == 1) return kv1_.get();
    if (leader == 2) return kv2_.get();
    return kv3_.get();
  }
};

TEST_F(IntegrationTest, LeaderElection) {
  std::this_thread::sleep_for(std::chrono::seconds(3));

  int64_t leader = GetLeader();
  EXPECT_TRUE(leader >= 1 && leader <= 3);

  grpc::ClientContext ctx;
  Empty req;
  MemberListResponse resp;
  auto status = cluster1_->MemberList(&ctx, req, &resp);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(resp.members_size(), 3);
}

TEST_F(IntegrationTest, WriteConsistency) {
  auto* leader = GetLeaderStub();

  grpc::ClientContext put_ctx;
  PutRequest put_req;
  put_req.set_key("test_key");
  put_req.set_value("test_value");
  PutResponse put_resp;
  auto status = leader->Put(&put_ctx, put_req, &put_resp);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(put_resp.header().code(), OK);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  for (auto* stub : {kv1_.get(), kv2_.get(), kv3_.get()}) {
    grpc::ClientContext get_ctx;
    GetRequest get_req;
    get_req.set_key("test_key");
    GetResponse get_resp;
    status = stub->Get(&get_ctx, get_req, &get_resp);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(get_resp.kvs_size(), 1);
    EXPECT_EQ(get_resp.kvs(0).value(), "test_value");
  }
}

TEST_F(IntegrationTest, ReadConsistency) {
  auto* leader = GetLeaderStub();

  grpc::ClientContext put_ctx;
  PutRequest put_req;
  put_req.set_key("read_test");
  put_req.set_value("read_value");
  PutResponse put_resp;
  leader->Put(&put_ctx, put_req, &put_resp);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  grpc::ClientContext get_ctx;
  GetRequest get_req;
  get_req.set_key("read_test");
  GetResponse get_resp;
  auto status = leader->Get(&get_ctx, get_req, &get_resp);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(get_resp.kvs_size(), 1);
  EXPECT_EQ(get_resp.kvs(0).value(), "read_value");
}

TEST_F(IntegrationTest, WatchEventStream) {
  auto* leader = GetLeaderStub();

  grpc::ClientContext watch_ctx;
  auto stream = watch1_->Watch(&watch_ctx);

  WatchRequest watch_req;
  auto* create = watch_req.mutable_create_request();
  create->set_key("watch_key");
  create->set_prefix(false);
  stream->Write(watch_req);

  WatchResponse watch_resp;
  stream->Read(&watch_resp);
  int64_t watch_id = watch_resp.watch_id();

  grpc::ClientContext put_ctx;
  PutRequest put_req;
  put_req.set_key("watch_key");
  put_req.set_value("watch_value");
  PutResponse put_resp;
  leader->Put(&put_ctx, put_req, &put_resp);

  EXPECT_TRUE(stream->Read(&watch_resp));
  EXPECT_EQ(watch_resp.events_size(), 1);
  EXPECT_EQ(watch_resp.events(0).type(), Event::PUT);
  EXPECT_EQ(watch_resp.events(0).kv().key(), "watch_key");

  stream->WritesDone();
}

TEST_F(IntegrationTest, LeaseExpiration) {
  grpc::ClientContext grant_ctx;
  LeaseGrantRequest grant_req;
  grant_req.set_ttl(2);
  LeaseGrantResponse grant_resp;
  auto status = lease1_->LeaseGrant(&grant_ctx, grant_req, &grant_resp);
  EXPECT_TRUE(status.ok());
  int64_t lease_id = grant_resp.id();

  auto* leader = GetLeaderStub();
  grpc::ClientContext put_ctx;
  PutRequest put_req;
  put_req.set_key("lease_key");
  put_req.set_value("lease_value");
  put_req.set_lease(lease_id);
  PutResponse put_resp;
  leader->Put(&put_ctx, put_req, &put_resp);

  std::this_thread::sleep_for(std::chrono::seconds(3));

  grpc::ClientContext get_ctx;
  GetRequest get_req;
  get_req.set_key("lease_key");
  GetResponse get_resp;
  leader->Get(&get_ctx, get_req, &get_resp);
  EXPECT_EQ(get_resp.kvs_size(), 0);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
