#pragma once

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/credentials.h>

#include <memory>
#include <string>

namespace etcdmvp {

class TlsConfig {
public:
  TlsConfig();

  bool IsEnabled() const { return enabled_; }
  bool IsMutualTlsEnabled() const { return mtls_enabled_; }

  std::shared_ptr<grpc::ServerCredentials> GetServerCredentials() const;
  std::shared_ptr<grpc::ChannelCredentials> GetClientCredentials() const;

  bool LoadFromEnvironment();
  bool LoadFromFiles(const std::string& cert_file,
                     const std::string& key_file,
                     const std::string& ca_file = "");

private:
  bool enabled_;
  bool mtls_enabled_;
  std::string cert_pem_;
  std::string key_pem_;
  std::string ca_pem_;
};

} // namespace etcdmvp
