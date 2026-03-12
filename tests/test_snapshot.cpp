#include "gtest/gtest.h"

#include "etcdmvp/storage/snapshot.h"
#include "etcdmvp/storage/config.h"

#include "tests/test_util.h"

#include <filesystem>

namespace etcdmvp {

TEST(SnapshotFileTest, SaveLoadRoundTrip) {
  StorageConfig cfg;
  cfg.data_dir = testutil::TempDir("snapshot_test");
  cfg.fsync_on_write = false;

  SnapshotFile snap(cfg);
  std::unordered_map<std::string, std::string> kv;
  kv["a"] = "1";
  kv["b"] = "2";

  ASSERT_TRUE(snap.Save(kv, 10, 2, "{\"node\":1}"));

  std::unordered_map<std::string, std::string> out;
  uint64_t rev = 0;
  uint64_t term = 0;
  std::string meta;
  ASSERT_TRUE(snap.Load(out, rev, term, meta));
  EXPECT_EQ(rev, 10u);
  EXPECT_EQ(term, 2u);
  EXPECT_EQ(meta, "{\"node\":1}");
  EXPECT_EQ(out.size(), 2u);
  EXPECT_EQ(out["a"], "1");
  EXPECT_EQ(out["b"], "2");

  testutil::RemoveAll(cfg.data_dir);
}

TEST(SnapshotFileTest, DetectCorruption) {
  StorageConfig cfg;
  cfg.data_dir = testutil::TempDir("snapshot_corrupt");
  cfg.fsync_on_write = false;

  SnapshotFile snap(cfg);
  std::unordered_map<std::string, std::string> kv;
  kv["k"] = "v";
  ASSERT_TRUE(snap.Save(kv, 1, 1, "{}"));

  std::filesystem::path path = std::filesystem::path(cfg.data_dir) / "snapshot.snap";
  auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size > 4 ? size - 4 : 1);

  std::unordered_map<std::string, std::string> out;
  uint64_t rev = 0;
  uint64_t term = 0;
  std::string meta;
  ASSERT_FALSE(snap.Load(out, rev, term, meta));
  EXPECT_EQ(snap.LastErrorCode(), "SNAPSHOT_CORRUPT");

  testutil::RemoveAll(cfg.data_dir);
}

} // namespace etcdmvp
