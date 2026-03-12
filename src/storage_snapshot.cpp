#include "etcdmvp/storage/snapshot.h"

#include <fstream>

namespace etcdmvp {

namespace {
void WriteU64(std::ostream& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xFF));
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
}

SnapshotFile::SnapshotFile(const std::string& path) : path_(path) {}

bool SnapshotFile::Save(const std::unordered_map<std::string, std::string>& kv, uint64_t revision) {
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  WriteU64(out, revision);
  WriteU64(out, static_cast<uint64_t>(kv.size()));
  for (const auto& it : kv) {
    WriteU64(out, static_cast<uint64_t>(it.first.size()));
    WriteU64(out, static_cast<uint64_t>(it.second.size()));
    out.write(it.first.data(), static_cast<std::streamsize>(it.first.size()));
    out.write(it.second.data(), static_cast<std::streamsize>(it.second.size()));
  }
  return out.good();
}

bool SnapshotFile::Load(std::unordered_map<std::string, std::string>& kv, uint64_t& revision) {
  std::ifstream in(path_, std::ios::binary);
  if (!in) return false;
  if (!ReadU64(in, revision)) return false;
  uint64_t count = 0;
  if (!ReadU64(in, count)) return false;
  kv.clear();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t klen = 0;
    uint64_t vlen = 0;
    if (!ReadU64(in, klen)) return false;
    if (!ReadU64(in, vlen)) return false;
    std::string key(klen, '\0');
    std::string value(vlen, '\0');
    if (!in.read(&key[0], static_cast<std::streamsize>(klen))) return false;
    if (!in.read(&value[0], static_cast<std::streamsize>(vlen))) return false;
    kv.emplace(std::move(key), std::move(value));
  }
  return true;
}

} // namespace etcdmvp
