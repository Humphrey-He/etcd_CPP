#include "gtest/gtest.h"

#include "etcdmvp/watch/watch_manager.h"
#include "tests/test_util.h"

#include <atomic>
#include <vector>

namespace etcdmvp {

TEST(WatchManagerTest, HistoryAndCompaction) {
  WatchManager wm(3);
  KvEvent e1{KvEvent::Type::Put, "k1", "v1", 1};
  KvEvent e2{KvEvent::Type::Put, "k2", "v2", 2};
  KvEvent e3{KvEvent::Type::Delete, "k1", "", 3};
  KvEvent e4{KvEvent::Type::Put, "k3", "v3", 4};

  wm.OnEvent(e1);
  wm.OnEvent(e2);
  wm.OnEvent(e3);
  wm.OnEvent(e4);

  EXPECT_EQ(wm.OldestRevision(), 2u);

  std::vector<KvEvent> history;
  EXPECT_FALSE(wm.GetHistory(1, history));
  EXPECT_TRUE(wm.GetHistory(2, history));
  ASSERT_EQ(history.size(), 3u);
  EXPECT_EQ(history.front().revision, 2u);
  EXPECT_EQ(history.back().revision, 4u);
}

TEST(WatchManagerTest, PersistHistory) {
  WatchManager wm(10);
  KvEvent e1{KvEvent::Type::Put, "k1", "v1", 1};
  KvEvent e2{KvEvent::Type::Delete, "k1", "", 2};
  wm.OnEvent(e1);
  wm.OnEvent(e2);

  std::string dir = testutil::TempDir("watch_hist");
  std::string path = dir + "/watch_history.bin";
  ASSERT_TRUE(wm.SaveHistory(path));

  WatchManager wm2(10);
  ASSERT_TRUE(wm2.LoadHistory(path));
  std::vector<KvEvent> history;
  ASSERT_TRUE(wm2.GetHistory(1, history));
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].type, KvEvent::Type::Put);
  EXPECT_EQ(history[1].type, KvEvent::Type::Delete);

  testutil::RemoveAll(dir);
}

TEST(WatchManagerTest, RegisterAndDispatch) {
  WatchManager wm(10);
  std::atomic<int> count{0};
  WatchRequest req;
  req.key = "p/";
  req.prefix = true;
  req.start_revision = 1;

  uint64_t id = wm.Register(req, [&](const KvEvent&) { count++; });
  wm.OnEvent(KvEvent{KvEvent::Type::Put, "p/a", "1", 1});
  wm.OnEvent(KvEvent{KvEvent::Type::Put, "x", "2", 2});
  wm.OnEvent(KvEvent{KvEvent::Type::Put, "p/b", "3", 3});
  wm.Unregister(id);

  EXPECT_EQ(count.load(), 2);
}

} // namespace etcdmvp
