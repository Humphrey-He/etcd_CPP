#include "etcdmvp/auth/auth_manager.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <sstream>

namespace etcdmvp {

namespace {

std::string SimpleHash(const std::string& input) {
  std::hash<std::string> hasher;
  size_t hash = hasher(input);
  std::ostringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(16) << hash;
  return ss.str();
}

} // namespace

AuthManager::AuthManager() : enabled_(false), token_seq_(1) {
  const char* env = std::getenv("ETCD_MVP_AUTH_ENABLED");
  if (env && std::string(env) == "true") {
    enabled_ = true;
  }
}

bool AuthManager::IsEnabled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return enabled_;
}

void AuthManager::Enable() {
  std::lock_guard<std::mutex> lock(mu_);
  enabled_ = true;
}

void AuthManager::Disable() {
  std::lock_guard<std::mutex> lock(mu_);
  enabled_ = false;
}

bool AuthManager::AddUser(const std::string& username, const std::string& password) {
  std::lock_guard<std::mutex> lock(mu_);
  if (users_.count(username)) return false;

  User user;
  user.username = username;
  user.password_hash = HashPassword(password);
  users_[username] = std::move(user);
  return true;
}

bool AuthManager::DeleteUser(const std::string& username) {
  std::lock_guard<std::mutex> lock(mu_);
  if (username == "root") return false;
  return users_.erase(username) > 0;
}

bool AuthManager::ChangePassword(const std::string& username, const std::string& password) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = users_.find(username);
  if (it == users_.end()) return false;

  it->second.password_hash = HashPassword(password);
  return true;
}

bool AuthManager::UserExists(const std::string& username) const {
  std::lock_guard<std::mutex> lock(mu_);
  return users_.count(username) > 0;
}

std::string AuthManager::Authenticate(const std::string& username, const std::string& password) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = users_.find(username);
  if (it == users_.end()) return "";

  if (!VerifyPassword(password, it->second.password_hash)) return "";

  std::string token = GenerateToken(username);
  tokens_[token] = username;
  return token;
}

bool AuthManager::ValidateToken(const std::string& token, std::string& username) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = tokens_.find(token);
  if (it == tokens_.end()) return false;
  username = it->second;
  return true;
}

bool AuthManager::AddRole(const std::string& role) {
  std::lock_guard<std::mutex> lock(mu_);
  if (roles_.count(role)) return false;

  Role r;
  r.name = role;
  roles_[role] = std::move(r);
  return true;
}

bool AuthManager::GrantRole(const std::string& username, const std::string& role) {
  std::lock_guard<std::mutex> lock(mu_);
  auto uit = users_.find(username);
  if (uit == users_.end()) return false;

  auto rit = roles_.find(role);
  if (rit == roles_.end()) return false;

  uit->second.roles.insert(role);
  return true;
}

bool AuthManager::GrantPermission(const std::string& role, PermissionType perm,
                                   const std::string& key, const std::string& range_end) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = roles_.find(role);
  if (it == roles_.end()) return false;

  Permission p;
  p.type = perm;
  p.key = key;
  p.range_end = range_end;
  it->second.permissions.push_back(std::move(p));
  return true;
}

bool AuthManager::CheckPermission(const std::string& username, PermissionType perm,
                                   const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!enabled_) return true;

  if (username == "root") return true;

  auto uit = users_.find(username);
  if (uit == users_.end()) return false;

  for (const auto& role_name : uit->second.roles) {
    auto rit = roles_.find(role_name);
    if (rit == roles_.end()) continue;

    for (const auto& p : rit->second.permissions) {
      if (p.type == PermissionType::READWRITE ||
          (perm == PermissionType::READ && (p.type == PermissionType::READ || p.type == PermissionType::READWRITE)) ||
          (perm == PermissionType::WRITE && (p.type == PermissionType::WRITE || p.type == PermissionType::READWRITE))) {
        if (KeyInRange(key, p.key, p.range_end)) {
          return true;
        }
      }
    }
  }

  return false;
}

void AuthManager::InitRootUser() {
  const char* root_pwd = std::getenv("ETCD_MVP_ROOT_PASSWORD");
  std::string password = root_pwd ? root_pwd : "root";

  AddUser("root", password);
  AddRole("root");
  GrantRole("root", "root");
  GrantPermission("root", PermissionType::READWRITE, "", "\xff");
}

std::string AuthManager::HashPassword(const std::string& password) const {
  return SimpleHash(password + "etcd_mvp_salt");
}

std::string AuthManager::GenerateToken(const std::string& username) const {
  auto now = std::chrono::system_clock::now().time_since_epoch().count();
  std::ostringstream ss;
  ss << "token-" << username << "-" << now << "-" << token_seq_++;
  return SimpleHash(ss.str());
}

bool AuthManager::VerifyPassword(const std::string& password, const std::string& hash) const {
  return HashPassword(password) == hash;
}

bool AuthManager::KeyInRange(const std::string& key, const std::string& start,
                              const std::string& end) const {
  if (start.empty() && end.empty()) return true;
  if (end.empty()) return key == start;
  return key >= start && key < end;
}

} // namespace etcdmvp
