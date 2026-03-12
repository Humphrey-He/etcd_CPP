#include "gtest/gtest.h"

#include "etcdmvp/kv/kv_engine.h"
#include "etcdmvp/watch/watch_manager.h"
#include "etcdmvp/lease/lease_manager.h"

#include <chrono>
#include <thread>

namespace etcdmvp {

namespace {
void WriteU8(std::string& out, uint8_t v) {
  out.push_back(static_cast<char>(v));
}

void WriteU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

void WriteU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

std::string EncodeTxn(const std::vector<std::tuple<uint8_t, std::string, std::string, uint64_t>>& ops) {
  std::string out;
  out.append("TXN1", 4);
  WriteU32(out, static_cast<uint32_t>(ops.size()));
  for (const auto& op : ops) {
    WriteU8(out, std::get<0>(op));
    WriteU32(out, static_cast<uint32_t>(std::get<1>(op).size()));
    out.append(std::get<1>(op));
    WriteU32(out, static_cast<uint32_t>(std::get<2>(op).size()));
    out.append(std::get<2>(op));
    WriteU64(out, std::get<3>(op));
  }
  return out;
}
}

TEST(KvEngineTest, PutGetDeleteMetadata) {
  WatchManager wm(10);
  LeaseManager lm(nullptr);
  KvEngine kv(&wm, &lm);

  lm.ApplyGrant(100, 10);
  uint64_t rev = 0;
  ASSERT_TRUE(kv.PutWithLease("k", "v", 100, &rev));
  EXPECT_EQ(rev, 1u);

  std::string value;
  ASSERT_TRUE(kv.Get("k", value));
  EXPECT_EQ(value, "v");

  uint64_t create_rev = 0;
  uint64_t mod_rev = 0;
  uint64_t version = 0;
  int64_t lease = 0;
  ASSERT_TRUE(kv.GetMetadata("k", create_rev, mod_rev, version, lease));
  EXPECT_EQ(create_rev, 1u);
  EXPECT_EQ(mod_rev, 1u);
  EXPECT_EQ(version, 1u);
  EXPECT_EQ(lease, 100);

  ASSERT_TRUE(kv.Delete("k", &rev));
  EXPECT_EQ(rev, 2u);
  EXPECT_FALSE(kv.Get("k", value));
}

TEST(KvEngineTest, ApplyTxnEntry) {
  WatchManager wm(10);
  LeaseManager lm(nullptr);
  KvEngine kv(&wm, &lm);

  std::vector<std::tuple<uint8_t, std::string, std::string, uint64_t>> ops;
  ops.emplace_back(1, "a", "1", 0);
  ops.emplace_back(1, "b", "2", 0);
  std::string cmd = EncodeTxn(ops);

  LogEntry entry;
  entry.index = 1;
  entry.term = 1;
  entry.command = cmd;
  kv.Apply(entry);

  std::string value;
  ASSERT_TRUE(kv.Get("a", value));
  EXPECT_EQ(value, "1");
  ASSERT_TRUE(kv.Get("b", value));
  EXPECT_EQ(value, "2");
}

} // namespace etcdmvp
