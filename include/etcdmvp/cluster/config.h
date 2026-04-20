#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace etcdmvp {

enum class ConfigChangeType {
  AddNode,
  RemoveNode
};

struct ConfigChange {
  ConfigChangeType type;
  int64_t node_id;
  std::string address;
};

class ClusterConfig {
public:
  ClusterConfig() = default;
  explicit ClusterConfig(const std::vector<int64_t>& members);

  bool IsMember(int64_t node_id) const;
  bool IsVoter(int64_t node_id) const;

  std::vector<int64_t> GetMembers() const;
  std::vector<int64_t> GetVoters() const;

  bool IsJointConsensus() const { return in_joint_consensus_; }

  void EnterJointConsensus(const ConfigChange& change);
  void FinalizeJointConsensus();

  bool HasQuorum(const std::unordered_set<int64_t>& voters) const;

  std::string GetAddress(int64_t node_id) const;
  void SetAddress(int64_t node_id, const std::string& address);

private:
  bool HasQuorumInConfig(const std::unordered_set<int64_t>& voters,
                         const std::unordered_set<int64_t>& config) const;

  std::unordered_set<int64_t> members_;
  std::unordered_set<int64_t> old_members_;
  std::unordered_set<int64_t> new_members_;
  std::unordered_map<int64_t, std::string> addresses_;

  bool in_joint_consensus_ = false;
};

} // namespace etcdmvp
