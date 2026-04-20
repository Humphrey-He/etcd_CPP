#include "etcdmvp/cluster/config.h"

namespace etcdmvp {

ClusterConfig::ClusterConfig(const std::vector<int64_t>& members) {
  for (int64_t id : members) {
    members_.insert(id);
  }
}

bool ClusterConfig::IsMember(int64_t node_id) const {
  if (in_joint_consensus_) {
    return old_members_.count(node_id) || new_members_.count(node_id);
  }
  return members_.count(node_id);
}

bool ClusterConfig::IsVoter(int64_t node_id) const {
  return IsMember(node_id);
}

std::vector<int64_t> ClusterConfig::GetMembers() const {
  if (in_joint_consensus_) {
    std::unordered_set<int64_t> all = old_members_;
    all.insert(new_members_.begin(), new_members_.end());
    return std::vector<int64_t>(all.begin(), all.end());
  }
  return std::vector<int64_t>(members_.begin(), members_.end());
}

std::vector<int64_t> ClusterConfig::GetVoters() const {
  return GetMembers();
}

void ClusterConfig::EnterJointConsensus(const ConfigChange& change) {
  old_members_ = members_;
  new_members_ = members_;

  if (change.type == ConfigChangeType::AddNode) {
    new_members_.insert(change.node_id);
    if (!change.address.empty()) {
      addresses_[change.node_id] = change.address;
    }
  } else {
    new_members_.erase(change.node_id);
  }

  in_joint_consensus_ = true;
}

void ClusterConfig::FinalizeJointConsensus() {
  if (!in_joint_consensus_) return;

  members_ = new_members_;
  old_members_.clear();
  new_members_.clear();
  in_joint_consensus_ = false;
}

bool ClusterConfig::HasQuorum(const std::unordered_set<int64_t>& voters) const {
  if (in_joint_consensus_) {
    return HasQuorumInConfig(voters, old_members_) &&
           HasQuorumInConfig(voters, new_members_);
  }
  return HasQuorumInConfig(voters, members_);
}

bool ClusterConfig::HasQuorumInConfig(const std::unordered_set<int64_t>& voters,
                                      const std::unordered_set<int64_t>& config) const {
  int count = 0;
  for (int64_t id : voters) {
    if (config.count(id)) count++;
  }
  return count > static_cast<int>(config.size()) / 2;
}

std::string ClusterConfig::GetAddress(int64_t node_id) const {
  auto it = addresses_.find(node_id);
  return it != addresses_.end() ? it->second : "";
}

void ClusterConfig::SetAddress(int64_t node_id, const std::string& address) {
  addresses_[node_id] = address;
}

} // namespace etcdmvp
