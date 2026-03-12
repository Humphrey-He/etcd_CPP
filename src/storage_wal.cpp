#include "etcdmvp/storage/wal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace etcdmvp {

namespace {
constexpr uint8_t kTypeEntry = 1;

void WriteU64(std::ostream& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xFF));
}

void WriteU32(std::ostream& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xFF));
}

bool ReadU64(std::istream& in, uint64_t& v) {
  v = 0;
  for (int i = 0; i < 8; ++i) {
    char c;
    if (!in.get(c)) return false;
    v |= (static_cast<uint64_t>(static_cast<unsigned char>(c)) << (i * 8));
  }
  return true;
}

bool ReadU32(std::istream& in, uint32_t& v) {
  v = 0;
  for (int i = 0; i < 4; ++i) {
    char c;
    if (!in.get(c)) return false;
    v |= (static_cast<uint32_t>(static_cast<unsigned char>(c)) << (i * 8));
  }
  return true;
}

uint32_t Crc32(const std::string& data) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }

  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char ch : data) {
    crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

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

std::string SegmentPath(const std::string& wal_dir, uint64_t start_index) {
  return wal_dir + "/wal_" + std::to_string(start_index) + ".log";
}

uint64_t ParseSegmentStart(const std::filesystem::path& path) {
  std::string name = path.filename().string();
  const std::string prefix = "wal_";
  const std::string suffix = ".log";
  if (name.rfind(prefix, 0) != 0) return 0;
  if (name.size() <= prefix.size() + suffix.size()) return 0;
  std::string num = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  try {
    return static_cast<uint64_t>(std::stoull(num));
  } catch (...) {
    return 0;
  }
}
}

WalFile::WalFile(const StorageConfig& config)
    : config_(config), wal_dir_(config.data_dir + "/wal"), hs_path_(config.data_dir + "/hardstate") {
  std::filesystem::create_directories(wal_dir_);
}

void WalFile::SetSnapshotBase(uint64_t index, uint64_t term) {
  std::lock_guard<std::mutex> lock(mu_);
  snapshot_base_index_ = index;
  snapshot_base_term_ = term;
}

bool WalFile::Append(const LogEntry& entry) {
  std::lock_guard<std::mutex> lock(mu_);
  return AppendEntry(entry);
}

bool WalFile::SaveHardState(const HardState& hs) {
  std::lock_guard<std::mutex> lock(mu_);
  std::filesystem::create_directories(config_.data_dir);
  std::ofstream out(hs_path_, std::ios::binary | std::ios::trunc);
  if (!out) {
    SetError("WAL_IO_ERROR", "hardstate open failed");
    return false;
  }
  WriteU64(out, hs.current_term);
  WriteU64(out, static_cast<uint64_t>(hs.voted_for));
  WriteU64(out, hs.commit_index);
  out.flush();
  if (!out.good()) {
    SetError("WAL_IO_ERROR", "hardstate write failed");
    return false;
  }
  if (config_.fsync_on_write && !FsyncFile(hs_path_)) {
    SetError("WAL_IO_ERROR", "hardstate fsync failed");
    return false;
  }
  last_error_code_.clear();
  last_error_message_.clear();
  return true;
}

bool WalFile::Load(std::vector<LogEntry>& out_log, HardState& out_hs) {
  std::lock_guard<std::mutex> lock(mu_);
  out_log.clear();
  out_log.push_back(LogEntry{snapshot_base_index_, snapshot_base_term_, ""});
  out_hs = HardState{};

  if (!ReadSegments(out_log)) {
    return false;
  }

  std::ifstream hs(hs_path_, std::ios::binary);
  if (hs) {
    ReadU64(hs, out_hs.current_term);
    uint64_t voted = 0;
    ReadU64(hs, voted);
    ReadU64(hs, out_hs.commit_index);
    out_hs.voted_for = static_cast<int64_t>(voted);
  }

  if (out_hs.commit_index < snapshot_base_index_) {
    out_hs.commit_index = snapshot_base_index_;
  }

  last_error_code_.clear();
  last_error_message_.clear();
  return true;
}

bool WalFile::PurgeUpTo(uint64_t index) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!std::filesystem::exists(wal_dir_)) return true;

  std::vector<std::filesystem::path> segments;
  for (auto& entry : std::filesystem::directory_iterator(wal_dir_)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().filename().string().rfind("wal_", 0) != 0) continue;
    segments.push_back(entry.path());
  }
  std::sort(segments.begin(), segments.end());

  for (const auto& seg : segments) {
    uint64_t last_index = 0;
    std::ifstream in(seg, std::ios::binary);
    if (!in) continue;
    while (true) {
      uint64_t term = 0;
      uint64_t idx = 0;
      uint8_t type = 0;
      uint32_t len = 0;
      uint32_t checksum = 0;
      if (!ReadU64(in, term)) break;
      if (!ReadU64(in, idx)) break;
      char t;
      if (!in.get(t)) break;
      type = static_cast<uint8_t>(t);
      if (!ReadU32(in, len)) break;
      if (!ReadU32(in, checksum)) break;
      std::string payload(len, '\0');
      if (!in.read(&payload[0], len)) break;
      if (type == kTypeEntry) {
        last_index = idx;
      }
    }
    if (last_index > 0 && last_index <= index) {
      std::filesystem::remove(seg);
    }
  }
  return true;
}

std::string WalFile::LastErrorCode() const { return last_error_code_; }
std::string WalFile::LastErrorMessage() const { return last_error_message_; }

bool WalFile::AppendEntry(const LogEntry& entry) {
  std::string payload = entry.command;
  uint32_t checksum = Crc32(payload);

  size_t record_size = 8 + 8 + 1 + 4 + 4 + payload.size();
  if (!EnsureSegment(entry.index, record_size)) {
    return false;
  }

  std::ofstream out(current_segment_path_, std::ios::binary | std::ios::app);
  if (!out) {
    SetError("WAL_IO_ERROR", "wal append open failed");
    return false;
  }

  WriteU64(out, entry.term);
  WriteU64(out, entry.index);
  out.put(static_cast<char>(kTypeEntry));
  WriteU32(out, static_cast<uint32_t>(payload.size()));
  WriteU32(out, checksum);
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  out.flush();

  if (!out.good()) {
    SetError("WAL_IO_ERROR", "wal append write failed");
    return false;
  }

  if (config_.fsync_on_write && !FsyncFile(current_segment_path_)) {
    SetError("WAL_IO_ERROR", "wal fsync failed");
    return false;
  }

  current_segment_size_ += record_size;
  last_error_code_.clear();
  last_error_message_.clear();
  return true;
}

bool WalFile::EnsureSegment(uint64_t entry_index, size_t record_size) {
  if (current_segment_path_.empty()) {
    current_segment_start_ = entry_index;
    current_segment_path_ = SegmentPath(wal_dir_, current_segment_start_);
    if (std::filesystem::exists(current_segment_path_)) {
      current_segment_size_ = static_cast<uint64_t>(std::filesystem::file_size(current_segment_path_));
    } else {
      current_segment_size_ = 0;
    }
    return true;
  }

  if (current_segment_size_ + record_size > config_.wal_segment_size) {
    current_segment_start_ = entry_index;
    current_segment_path_ = SegmentPath(wal_dir_, current_segment_start_);
    current_segment_size_ = 0;
  }
  return true;
}

bool WalFile::ReadSegments(std::vector<LogEntry>& out_log) {
  std::vector<std::filesystem::path> segments;
  if (std::filesystem::exists(wal_dir_)) {
    for (auto& entry : std::filesystem::directory_iterator(wal_dir_)) {
      if (!entry.is_regular_file()) continue;
      if (entry.path().filename().string().rfind("wal_", 0) != 0) continue;
      segments.push_back(entry.path());
    }
  }

  std::sort(segments.begin(), segments.end(), [](const auto& a, const auto& b) {
    return ParseSegmentStart(a) < ParseSegmentStart(b);
  });

  uint64_t last_index = snapshot_base_index_;
  for (const auto& seg : segments) {
    std::ifstream in(seg, std::ios::binary);
    if (!in) {
      SetError("WAL_IO_ERROR", "wal segment open failed");
      return false;
    }

    while (true) {
      uint64_t term = 0;
      uint64_t idx = 0;
      uint8_t type = 0;
      uint32_t len = 0;
      uint32_t checksum = 0;
      if (!ReadU64(in, term)) {
        if (in.eof()) break;
        SetError("WAL_CORRUPT", "wal header term missing");
        return false;
      }
      if (!ReadU64(in, idx)) {
        SetError("WAL_CORRUPT", "wal header index missing");
        return false;
      }
      char t;
      if (!in.get(t)) {
        SetError("WAL_CORRUPT", "wal header type missing");
        return false;
      }
      type = static_cast<uint8_t>(t);
      if (!ReadU32(in, len)) {
        SetError("WAL_CORRUPT", "wal header length missing");
        return false;
      }
      if (!ReadU32(in, checksum)) {
        SetError("WAL_CORRUPT", "wal checksum missing");
        return false;
      }
      std::string payload(len, '\0');
      if (!in.read(&payload[0], len)) {
        SetError("WAL_CORRUPT", "wal payload truncated");
        return false;
      }
      if (type != kTypeEntry) {
        continue;
      }
      if (Crc32(payload) != checksum) {
        SetError("WAL_CORRUPT", "wal checksum mismatch");
        return false;
      }
      if (idx <= snapshot_base_index_) {
        continue;
      }
      if (idx <= last_index) {
        SetError("WAL_CORRUPT", "wal index out of order");
        return false;
      }
      last_index = idx;
      out_log.push_back(LogEntry{idx, term, payload});
    }
  }

  last_error_code_.clear();
  last_error_message_.clear();
  return true;
}

void WalFile::SetError(const std::string& code, const std::string& message) {
  last_error_code_ = code;
  last_error_message_ = message;
}

} // namespace etcdmvp
