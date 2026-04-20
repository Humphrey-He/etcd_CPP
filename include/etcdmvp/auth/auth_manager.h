#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace etcdmvp {

enum class PermissionType {
  READ = 0,
  WRITE = 1,
  READWRITE = 2
};

struct Permission {
  PermissionType type;
  std::string key;
  std::string range_end;
};

struct Role {
  std::string name;
  std::vector<Permission> permissions;
};

struct User {
  std::string username;
  std::string password_hash;
  std::unordered_set<std::string> roles;
};

class AuthManager {
public:
  AuthManager();

  bool IsEnabled() const;
  void Enable();
  void Disable();

  bool AddUser(const std::string& username, const std::string& password);
  bool DeleteUser(const std::string& username);
  bool ChangePassword(const std::string& username, const std::string& password);
  bool UserExists(const std::string& username) const;

  std::string Authenticate(const std::string& username, const std::string& password);
  bool ValidateToken(const std::string& token, std::string& username) const;

  bool AddRole(const std::string& role);
  bool GrantRole(const std::string& username, const std::string& role);
  bool GrantPermission(const std::string& role, PermissionType perm,
                       const std::string& key, const std::string& range_end);

  bool CheckPermission(const std::string& username, PermissionType perm,
                       const std::string& key) const;

  void InitRootUser();

private:
  std::string HashPassword(const std::string& password) const;
  std::string GenerateToken(const std::string& username) const;
  bool VerifyPassword(const std::string& password, const std::string& hash) const;
  bool KeyInRange(const std::string& key, const std::string& start,
                  const std::string& end) const;

  mutable std::mutex mu_;
  bool enabled_;
  std::unordered_map<std::string, User> users_;
  std::unordered_map<std::string, Role> roles_;
  std::unordered_map<std::string, std::string> tokens_;
  uint64_t token_seq_;
};

} // namespace etcdmvp
