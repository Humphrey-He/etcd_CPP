#include "etcdmvp/storage/backend.h"

#include <cstdlib>

#ifdef ETCD_MVP_USE_ROCKSDB
#include <rocksdb/options.h>
#endif

namespace etcdmvp {

bool MemoryBackend::Put(const std::string& key, const std::string& value) {
  data_[key] = value;
  return true;
}

bool MemoryBackend::Get(const std::string& key, std::string& value) const {
  auto it = data_.find(key);
  if (it == data_.end()) return false;
  value = it->second;
  return true;
}

bool MemoryBackend::Delete(const std::string& key) {
  return data_.erase(key) > 0;
}

bool MemoryBackend::Exists(const std::string& key) const {
  return data_.count(key) > 0;
}

void MemoryBackend::Snapshot(std::unordered_map<std::string, std::string>& out) const {
  out = data_;
}

void MemoryBackend::LoadSnapshot(const std::unordered_map<std::string, std::string>& data) {
  data_ = data;
}

void MemoryBackend::Clear() {
  data_.clear();
}

#ifdef ETCD_MVP_USE_ROCKSDB
RocksDBBackend::RocksDBBackend(const std::string& path) {
  rocksdb::Options options;
  options.create_if_missing = true;
  rocksdb::Status status = rocksdb::DB::Open(options, path, &db_);
  if (!status.ok()) {
    db_ = nullptr;
  }
}

RocksDBBackend::~RocksDBBackend() {
  delete db_;
}

bool RocksDBBackend::Put(const std::string& key, const std::string& value) {
  if (!db_) return false;
  rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), key, value);
  return s.ok();
}

bool RocksDBBackend::Get(const std::string& key, std::string& value) const {
  if (!db_) return false;
  rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &value);
  return s.ok();
}

bool RocksDBBackend::Delete(const std::string& key) {
  if (!db_) return false;
  rocksdb::Status s = db_->Delete(rocksdb::WriteOptions(), key);
  return s.ok();
}

bool RocksDBBackend::Exists(const std::string& key) const {
  std::string value;
  return Get(key, value);
}

void RocksDBBackend::Snapshot(std::unordered_map<std::string, std::string>& out) const {
  if (!db_) return;
  out.clear();
  rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out[it->key().ToString()] = it->value().ToString();
  }
  delete it;
}

void RocksDBBackend::LoadSnapshot(const std::unordered_map<std::string, std::string>& data) {
  if (!db_) return;
  Clear();
  for (const auto& kv : data) {
    Put(kv.first, kv.second);
  }
}

void RocksDBBackend::Clear() {
  if (!db_) return;
  rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    db_->Delete(rocksdb::WriteOptions(), it->key());
  }
  delete it;
}
#endif

std::unique_ptr<IStorageBackend> CreateStorageBackend(const std::string& type, const std::string& path) {
  if (type == "rocksdb") {
#ifdef ETCD_MVP_USE_ROCKSDB
    return std::make_unique<RocksDBBackend>(path);
#else
    return std::make_unique<MemoryBackend>();
#endif
  }
  return std::make_unique<MemoryBackend>();
}

} // namespace etcdmvp
