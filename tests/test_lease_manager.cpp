#include "gtest/gtest.h"

#include "etcdmvp/lease/lease_manager.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace etcdmvp {

TEST(LeaseManagerTest, GrantKeepAliveRevoke) {
  std::vector<std::string> expired;
  LeaseManager lm([&](const std::string& key) { expired.push_back(key); });

  int64_t id = lm.Grant(5);
  lm.AttachKey(id, "k1");
  EXPECT_EQ(lm.LeaseOfKey("k1"), id);

  int64_t ttl = 0;
  ASSERT_TRUE(lm.KeepAlive(id, 7, ttl));
  EXPECT_EQ(ttl, 7);

  ASSERT_TRUE(lm.Revoke(id));
  EXPECT_EQ(lm.LeaseOfKey("k1"), 0);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0], "k1");
}

TEST(LeaseManagerTest, ExpireTriggersCallback) {
  std::atomic<int> fired{0};
  LeaseManager lm([&](const std::string&) { fired++; });
  int64_t id = lm.Grant(1);
  lm.AttachKey(id, "k1");
  lm.Start();

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fired.load() > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  lm.Stop();

  EXPECT_GT(fired.load(), 0);
}

} // namespace etcdmvp
