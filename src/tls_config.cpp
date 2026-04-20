#include "etcdmvp/tls/tls_config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace etcdmvp {

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return "";
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

} // namespace

TlsConfig::TlsConfig() : enabled_(false), mtls_enabled_(false) {}

std::shared_ptr<grpc::ServerCredentials> TlsConfig::GetServerCredentials() const {
  if (!enabled_) {
    return grpc::InsecureServerCredentials();
  }

  grpc::SslServerCredentialsOptions ssl_opts;
  ssl_opts.pem_key_cert_pairs.push_back({key_pem_, cert_pem_});

  if (mtls_enabled_ && !ca_pem_.empty()) {
    ssl_opts.pem_root_certs = ca_pem_;
    ssl_opts.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  }

  return grpc::SslServerCredentials(ssl_opts);
}

std::shared_ptr<grpc::ChannelCredentials> TlsConfig::GetClientCredentials() const {
  if (!enabled_) {
    return grpc::InsecureChannelCredentials();
  }

  grpc::SslCredentialsOptions ssl_opts;
  ssl_opts.pem_root_certs = ca_pem_.empty() ? cert_pem_ : ca_pem_;

  if (mtls_enabled_) {
    ssl_opts.pem_cert_chain = cert_pem_;
    ssl_opts.pem_private_key = key_pem_;
  }

  return grpc::SslCredentials(ssl_opts);
}

bool TlsConfig::LoadFromEnvironment() {
  const char* cert_path = std::getenv("ETCD_MVP_TLS_CERT");
  const char* key_path = std::getenv("ETCD_MVP_TLS_KEY");
  const char* ca_path = std::getenv("ETCD_MVP_TLS_CA");

  if (!cert_path || !key_path) {
    enabled_ = false;
    return true;
  }

  return LoadFromFiles(cert_path, key_path, ca_path ? ca_path : "");
}

bool TlsConfig::LoadFromFiles(const std::string& cert_file,
                               const std::string& key_file,
                               const std::string& ca_file) {
  cert_pem_ = ReadFile(cert_file);
  key_pem_ = ReadFile(key_file);

  if (cert_pem_.empty() || key_pem_.empty()) {
    enabled_ = false;
    return false;
  }

  enabled_ = true;

  if (!ca_file.empty()) {
    ca_pem_ = ReadFile(ca_file);
    mtls_enabled_ = !ca_pem_.empty();
  }

  return true;
}

} // namespace etcdmvp
