#include "etcd_mvp.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

class AuthClient {
public:
  AuthClient(std::shared_ptr<Channel> channel)
      : kv_stub_(etcdmvp::KV::NewStub(channel)),
        auth_stub_(etcdmvp::Auth::NewStub(channel)) {}

  std::string Authenticate(const std::string& username, const std::string& password) {
    etcdmvp::AuthenticateRequest request;
    request.set_username(username);
    request.set_password(password);

    etcdmvp::AuthenticateResponse response;
    ClientContext context;

    Status status = auth_stub_->Authenticate(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      std::cout << "Authentication successful" << std::endl;
      return response.token();
    } else {
      std::cerr << "Authentication failed: " << response.header().message() << std::endl;
      return "";
    }
  }

  void Put(const std::string& token, const std::string& key, const std::string& value) {
    etcdmvp::PutRequest request;
    request.set_key(key);
    request.set_value(value);

    etcdmvp::PutResponse response;
    ClientContext context;
    context.AddMetadata("token", token);

    Status status = kv_stub_->Put(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      std::cout << "Put successful, revision: " << response.revision() << std::endl;
    } else {
      std::cerr << "Put failed: " << response.header().message() << std::endl;
    }
  }

  void Get(const std::string& token, const std::string& key) {
    etcdmvp::GetRequest request;
    request.set_key(key);

    etcdmvp::GetResponse response;
    ClientContext context;
    context.AddMetadata("token", token);

    Status status = kv_stub_->Get(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      if (response.kvs_size() > 0) {
        std::cout << "Get successful:" << std::endl;
        std::cout << "  Key: " << response.kvs(0).key() << std::endl;
        std::cout << "  Value: " << response.kvs(0).value() << std::endl;
        std::cout << "  Revision: " << response.revision() << std::endl;
      } else {
        std::cout << "Key not found" << std::endl;
      }
    } else {
      std::cerr << "Get failed: " << response.header().message() << std::endl;
    }
  }

  void AddUser(const std::string& admin_token, const std::string& username,
               const std::string& password) {
    etcdmvp::UserAddRequest request;
    request.set_username(username);
    request.set_password(password);
    request.set_token(admin_token);

    etcdmvp::UserAddResponse response;
    ClientContext context;
    context.AddMetadata("token", admin_token);

    Status status = auth_stub_->UserAdd(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      std::cout << "User added successfully" << std::endl;
    } else {
      std::cerr << "Add user failed: " << response.header().message() << std::endl;
    }
  }

  void AddRole(const std::string& admin_token, const std::string& role) {
    etcdmvp::RoleAddRequest request;
    request.set_name(role);
    request.set_token(admin_token);

    etcdmvp::RoleAddResponse response;
    ClientContext context;
    context.AddMetadata("token", admin_token);

    Status status = auth_stub_->RoleAdd(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      std::cout << "Role added successfully" << std::endl;
    } else {
      std::cerr << "Add role failed: " << response.header().message() << std::endl;
    }
  }

  void GrantPermission(const std::string& admin_token, const std::string& role,
                       etcdmvp::Permission perm, const std::string& key,
                       const std::string& range_end) {
    etcdmvp::RoleGrantPermissionRequest request;
    request.set_role(role);
    request.set_permission(perm);
    request.set_key(key);
    request.set_range_end(range_end);
    request.set_token(admin_token);

    etcdmvp::RoleGrantPermissionResponse response;
    ClientContext context;
    context.AddMetadata("token", admin_token);

    Status status = auth_stub_->RoleGrantPermission(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      std::cout << "Permission granted successfully" << std::endl;
    } else {
      std::cerr << "Grant permission failed: " << response.header().message() << std::endl;
    }
  }

  void GrantRole(const std::string& admin_token, const std::string& username,
                 const std::string& role) {
    etcdmvp::UserGrantRoleRequest request;
    request.set_username(username);
    request.set_role(role);
    request.set_token(admin_token);

    etcdmvp::UserGrantRoleResponse response;
    ClientContext context;
    context.AddMetadata("token", admin_token);

    Status status = auth_stub_->UserGrantRole(&context, request, &response);

    if (status.ok() && response.header().code() == etcdmvp::OK) {
      std::cout << "Role granted to user successfully" << std::endl;
    } else {
      std::cerr << "Grant role failed: " << response.header().message() << std::endl;
    }
  }

private:
  std::unique_ptr<etcdmvp::KV::Stub> kv_stub_;
  std::unique_ptr<etcdmvp::Auth::Stub> auth_stub_;
};

int main(int argc, char** argv) {
  std::string target = "localhost:2379";
  if (argc > 1) {
    target = argv[1];
  }

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  AuthClient client(channel);

  std::cout << "=== Authentication Example ===" << std::endl;
  std::string root_token = client.Authenticate("root", "root");
  if (root_token.empty()) {
    std::cerr << "Failed to authenticate as root" << std::endl;
    return 1;
  }

  std::cout << "\n=== Create User ===" << std::endl;
  client.AddUser(root_token, "alice", "alice_password");

  std::cout << "\n=== Create Role ===" << std::endl;
  client.AddRole(root_token, "reader");

  std::cout << "\n=== Grant Permission ===" << std::endl;
  client.GrantPermission(root_token, "reader", etcdmvp::READ, "/app/", "/app0");

  std::cout << "\n=== Grant Role to User ===" << std::endl;
  client.GrantRole(root_token, "alice", "reader");

  std::cout << "\n=== Put with Root Token ===" << std::endl;
  client.Put(root_token, "/app/config", "value1");

  std::cout << "\n=== Authenticate as Alice ===" << std::endl;
  std::string alice_token = client.Authenticate("alice", "alice_password");

  if (!alice_token.empty()) {
    std::cout << "\n=== Get with Alice Token (should succeed) ===" << std::endl;
    client.Get(alice_token, "/app/config");

    std::cout << "\n=== Put with Alice Token (should fail - no write permission) ===" << std::endl;
    client.Put(alice_token, "/app/config", "value2");
  }

  return 0;
}
