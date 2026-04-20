#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace etcdmvp {

class IStorageBackend {
public:
  virtual ~IStorageBackend() = default;

  virtual bool Put(const std::string& key, const std::string& value) = 0;
  virtual bool Get(const std::string& key, std::string& value) const = 0;
  virtual bool Delete(const std::string& key) = 0;
  virtual bool Exists(const std::string& key) const = 0;

  virtual void Snapshot(std::unordered_map<std::string, std::string>& out) const = 0;
  virtual void LoadSnapshot(const std::unordered_map<std::string, std::string>& data) = 0;

  virtual void Clear() = 0;
};

class MemoryBackend : public IStorageBackend {
public:
  bool Put(const std::string& key, const std::string& value) override;
  bool Get(const std::string& key, std::string& value) const override;
  bool Delete(const std::string& key) override;
  bool Exists(const std::string& key) const override;

  void Snapshot(std::unordered_map<std::string, std::string>& out) const override;
  void LoadSnapshot(const std::unordered_map<std::string, std::string>& data) override;

  void Clear() override;

private:
  std::unordered_map<std::string, std::string> data_;
};

#ifdef ETCD_MVP_USE_ROCKSDB
#include <rocksdb/db.h>

class RocksDBBackend : public IStorageBackend {
public:
  explicit RocksDBBackend(const std::string& path);
  ~RocksDBBackend() override;

  bool Put(const std::string& key, const std::string& value) override;
  bool Get(const std::string& key, std::string& value) const override;
  bool Delete(const std::string& key) override;
  bool Exists(const std::string& key) const override;

  void Snapshot(std::unordered_map<std::string, std::string>& out) const override;
  void LoadSnapshot(const std::unordered_map<std::string, std::string>& data) override;

  void Clear() override;

private:
  rocksdb::DB* db_ = nullptr;
};
#endif

std::unique_ptr<IStorageBackend> CreateStorageBackend(const std::string& type, const std::string& path);

} // namespace etcdmvp
