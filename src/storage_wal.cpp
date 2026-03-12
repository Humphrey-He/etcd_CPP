#include "etcdmvp/storage/wal.h"

#include <fstream>
#include <sstream>

namespace etcdmvp {

namespace {
void WriteU64(std::string& buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) buf.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

void WriteU32(std::string& buf, uint32_t v) {
  for (int i = 0; i < 4; ++i) buf.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
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

std::string SerializeEntry(const LogEntry& e) {
  std::string buf;
  WriteU64(buf, e.index);
  WriteU64(buf, e.term);
  WriteU32(buf, static_cast<uint32_t>(e.command.size()));
  buf.append(e.command);
  return buf;
}

bool DeserializeEntry(std::istream& in, LogEntry& e) {
  if (!ReadU64(in, e.index)) return false;
  if (!ReadU64(in, e.term)) return false;
  uint32_t len = 0;
  if (!ReadU32(in, len)) return false;
  std::string cmd(len, '\0');
  if (!in.read(&cmd[0], len)) return false;
  e.command = std::move(cmd);
  return true;
}

std::string SerializeHardState(const HardState& hs) {
  std::string buf;
  WriteU64(buf, hs.current_term);
  WriteU64(buf, static_cast<uint64_t>(hs.voted_for));
  WriteU64(buf, hs.commit_index);
  return buf;
}

bool DeserializeHardState(std::istream& in, HardState& hs) {
  uint64_t voted = 0;
  if (!ReadU64(in, hs.current_term)) return false;
  if (!ReadU64(in, voted)) return false;
  if (!ReadU64(in, hs.commit_index)) return false;
  hs.voted_for = static_cast<int64_t>(voted);
  return true;
}
}

WalFile::WalFile(const std::string& path_prefix)
    : wal_path_(path_prefix + ".wal"), hs_path_(path_prefix + ".hs") {}

bool WalFile::Append(const LogEntry& entry) {
  std::lock_guard<std::mutex> lock(mu_);
  return AppendRecord('E', SerializeEntry(entry));
}

bool WalFile::SaveHardState(const HardState& hs) {
  std::lock_guard<std::mutex> lock(mu_);
  std::ofstream out(hs_path_, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  std::string data = SerializeHardState(hs);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

bool WalFile::Load(std::vector<LogEntry>& out_log, HardState& out_hs) {
  std::lock_guard<std::mutex> lock(mu_);
  out_log.clear();
  out_log.push_back(LogEntry{});
  out_hs = HardState{};

  ReadRecords(out_log, out_hs);

  std::ifstream hs(hs_path_, std::ios::binary);
  if (hs) {
    DeserializeHardState(hs, out_hs);
  }
  return true;
}

bool WalFile::AppendRecord(char type, const std::string& payload) {
  std::ofstream out(wal_path_, std::ios::binary | std::ios::app);
  if (!out) return false;
  uint32_t len = static_cast<uint32_t>(payload.size());
  out.put(type);
  for (int i = 0; i < 4; ++i) {
    out.put(static_cast<char>((len >> (i * 8)) & 0xFF));
  }
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  return out.good();
}

bool WalFile::ReadRecords(std::vector<LogEntry>& out_log, HardState& out_hs) {
  std::ifstream in(wal_path_, std::ios::binary);
  if (!in) return false;
  while (true) {
    char type;
    if (!in.get(type)) break;
    uint32_t len = 0;
    if (!ReadU32(in, len)) break;
    std::string payload(len, '\0');
    if (!in.read(&payload[0], len)) break;
    std::istringstream ss(payload);
    if (type == 'E') {
      LogEntry e;
      if (DeserializeEntry(ss, e)) {
        out_log.push_back(e);
      }
    } else if (type == 'H') {
      DeserializeHardState(ss, out_hs);
    }
  }
  return true;
}

} // namespace etcdmvp
