#include "gtest/gtest.h"

#include "etcdmvp/storage/wal.h"
#include "etcdmvp/storage/config.h"

#include "tests/test_util.h"

#include <filesystem>

namespace etcdmvp {

TEST(WalFileTest, AppendLoadAndRotate) {
  StorageConfig cfg;
  cfg.data_dir = testutil::TempDir("wal_test");
  cfg.wal_segment_size = 200;
  cfg.fsync_on_write = false;

  WalFile wal(cfg);
  for (uint64_t i = 1; i <= 10; ++i) {
    LogEntry e;
    e.index = i;
    e.term = 1;
    e.command = std::string(20, 'a' + static_cast<char>(i % 10));
    ASSERT_TRUE(wal.Append(e));
  }

  std::filesystem::path wal_dir = std::filesystem::path(cfg.data_dir) / "wal";
  size_t seg_count = 0;
  for (const auto& it : std::filesystem::directory_iterator(wal_dir)) {
    if (it.is_regular_file()) seg_count++;
  }
  EXPECT_GT(seg_count, 1u);

  std::vector<LogEntry> out_log;
  HardState hs;
  ASSERT_TRUE(wal.Load(out_log, hs));
  ASSERT_EQ(out_log.size(), 11u);
  EXPECT_EQ(out_log.front().index, 0u);
  EXPECT_EQ(out_log.back().index, 10u);

  testutil::RemoveAll(cfg.data_dir);
}

TEST(WalFileTest, DetectCorruption) {
  StorageConfig cfg;
  cfg.data_dir = testutil::TempDir("wal_corrupt");
  cfg.wal_segment_size = 1024 * 1024;
  cfg.fsync_on_write = false;

  WalFile wal(cfg);
  LogEntry e;
  e.index = 1;
  e.term = 1;
  e.command = "hello";
  ASSERT_TRUE(wal.Append(e));

  std::filesystem::path wal_dir = std::filesystem::path(cfg.data_dir) / "wal";
  std::filesystem::path seg;
  for (const auto& it : std::filesystem::directory_iterator(wal_dir)) {
    if (it.is_regular_file()) {
      seg = it.path();
      break;
    }
  }
  ASSERT_FALSE(seg.empty());

  auto size = std::filesystem::file_size(seg);
  std::filesystem::resize_file(seg, size > 4 ? size - 4 : 1);

  std::vector<LogEntry> out_log;
  HardState hs;
  ASSERT_FALSE(wal.Load(out_log, hs));
  EXPECT_EQ(wal.LastErrorCode(), "WAL_CORRUPT");

  testutil::RemoveAll(cfg.data_dir);
}

} // namespace etcdmvp
