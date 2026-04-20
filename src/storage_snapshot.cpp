#include "etcdmvp/storage/snapshot.h"

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace etcdmvp {

namespace {
// Serialize uint64_t to stream in little-endian format
void WriteU64(std::ostream& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xFF));
}

// Deserialize uint64_t from stream in little-endian format
bool ReadU64(std::istream& in, uint64_t& v) {
  v = 0;
  for (int i = 0; i < 8; ++i) {
    char c;
    if (!in.get(c)) return false;
    v |= (static_cast<uint64_t>(static_cast<unsigned char>(c)) << (i * 8));
  }
  return true;
}

// Force file data to disk (platform-specific fsync)
bool FsyncFile(const std::string& path) {
#ifdef _WIN32
  int fd = _open(path.c_str(), _O_BINARY | _O_RDWR);
  if (fd < 0) return false;
  int rc = _commit(fd);
  _close(fd);
  return rc == 0;
#else
  int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) return false;
  int rc = ::fsync(fd);
  ::close(fd);
  return rc == 0;
#endif
}
}

SnapshotFile::SnapshotFile(const StorageConfig& config)
    : config_(config), path_(config.data_dir + "/snapshot.snap") {}

// Save KV state to snapshot file atomically (write to .tmp then rename)
bool SnapshotFile::Save(const std::unordered_map<std::string, std::string>& kv,
                        uint64_t revision,
                        uint64_t last_term,
                        const std::string& metadata_json) {
  std::filesystem::create_directories(config_.data_dir);
  std::string tmp = path_ + ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) {
    SetError("SNAPSHOT_IO_ERROR", "cannot open snapshot tmp");
    return false;
  }

  WriteU64(out, revision);
  WriteU64(out, last_term);
  WriteU64(out, static_cast<uint64_t>(kv.size()));
  for (const auto& it : kv) {
    WriteU64(out, static_cast<uint64_t>(it.first.size()));
    WriteU64(out, static_cast<uint64_t>(it.second.size()));
    out.write(it.first.data(), static_cast<std::streamsize>(it.first.size()));
    out.write(it.second.data(), static_cast<std::streamsize>(it.second.size()));
  }
  WriteU64(out, static_cast<uint64_t>(metadata_json.size()));
  out.write(metadata_json.data(), static_cast<std::streamsize>(metadata_json.size()));
  out.flush();

  if (!out.good()) {
    SetError("SNAPSHOT_IO_ERROR", "snapshot write failed");
    return false;
  }
  out.close();

  if (config_.fsync_on_write && !FsyncFile(tmp)) {
    SetError("SNAPSHOT_IO_ERROR", "snapshot fsync failed");
    return false;
  }

  if (std::filesystem::exists(path_)) {
    std::filesystem::remove(path_);
  }
  std::filesystem::rename(tmp, path_);

  last_error_code_.clear();
  last_error_message_.clear();
  return true;
}

// Load KV state from snapshot file
bool SnapshotFile::Load(std::unordered_map<std::string, std::string>& kv,
                        uint64_t& revision,
                        uint64_t& last_term,
                        std::string& metadata_json) {
  if (!std::filesystem::exists(path_)) {
    SetError("SNAPSHOT_NOT_FOUND", "snapshot not found");
    return false;
  }

  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    SetError("SNAPSHOT_IO_ERROR", "snapshot open failed");
    return false;
  }

  if (!ReadU64(in, revision)) {
    SetError("SNAPSHOT_CORRUPT", "snapshot read revision failed");
    return false;
  }
  if (!ReadU64(in, last_term)) {
    SetError("SNAPSHOT_CORRUPT", "snapshot read term failed");
    return false;
  }
  uint64_t count = 0;
  if (!ReadU64(in, count)) {
    SetError("SNAPSHOT_CORRUPT", "snapshot read kv count failed");
    return false;
  }

  kv.clear();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t klen = 0;
    uint64_t vlen = 0;
    if (!ReadU64(in, klen) || !ReadU64(in, vlen)) {
      SetError("SNAPSHOT_CORRUPT", "snapshot read kv lens failed");
      return false;
    }
    std::string key(klen, '\0');
    std::string value(vlen, '\0');
    if (!in.read(&key[0], static_cast<std::streamsize>(klen))) {
      SetError("SNAPSHOT_CORRUPT", "snapshot read key failed");
      return false;
    }
    if (!in.read(&value[0], static_cast<std::streamsize>(vlen))) {
      SetError("SNAPSHOT_CORRUPT", "snapshot read value failed");
      return false;
    }
    kv.emplace(std::move(key), std::move(value));
  }

  uint64_t meta_len = 0;
  if (!ReadU64(in, meta_len)) {
    SetError("SNAPSHOT_CORRUPT", "snapshot read metadata len failed");
    return false;
  }
  metadata_json.assign(meta_len, '\0');
  if (!in.read(&metadata_json[0], static_cast<std::streamsize>(meta_len))) {
    SetError("SNAPSHOT_CORRUPT", "snapshot read metadata failed");
    return false;
  }

  last_error_code_.clear();
  last_error_message_.clear();
  return true;
}

std::string SnapshotFile::LastErrorCode() const { return last_error_code_; }
std::string SnapshotFile::LastErrorMessage() const { return last_error_message_; }

void SnapshotFile::SetError(const std::string& code, const std::string& message) {
  last_error_code_ = code;
  last_error_message_ = message;
}

} // namespace etcdmvp
