#!/bin/bash

set -e

CERT_DIR="${1:-./certs}"
mkdir -p "$CERT_DIR"

echo "Generating certificates in $CERT_DIR..."

# Generate CA private key
openssl genrsa -out "$CERT_DIR/ca-key.pem" 4096

# Generate CA certificate
openssl req -new -x509 -days 3650 -key "$CERT_DIR/ca-key.pem" \
  -out "$CERT_DIR/ca-cert.pem" \
  -subj "/CN=etcd-mvp-ca"

# Generate server private key
openssl genrsa -out "$CERT_DIR/server-key.pem" 4096

# Generate server CSR
openssl req -new -key "$CERT_DIR/server-key.pem" \
  -out "$CERT_DIR/server.csr" \
  -subj "/CN=localhost"

# Create server certificate extensions file
cat > "$CERT_DIR/server-ext.cnf" <<EOF
subjectAltName = DNS:localhost,IP:127.0.0.1
EOF

# Sign server certificate
openssl x509 -req -days 3650 \
  -in "$CERT_DIR/server.csr" \
  -CA "$CERT_DIR/ca-cert.pem" \
  -CAkey "$CERT_DIR/ca-key.pem" \
  -CAcreateserial \
  -out "$CERT_DIR/server-cert.pem" \
  -extfile "$CERT_DIR/server-ext.cnf"

# Generate client private key (for mTLS)
openssl genrsa -out "$CERT_DIR/client-key.pem" 4096

# Generate client CSR
openssl req -new -key "$CERT_DIR/client-key.pem" \
  -out "$CERT_DIR/client.csr" \
  -subj "/CN=etcd-mvp-client"

# Sign client certificate
openssl x509 -req -days 3650 \
  -in "$CERT_DIR/client.csr" \
  -CA "$CERT_DIR/ca-cert.pem" \
  -CAkey "$CERT_DIR/ca-key.pem" \
  -CAcreateserial \
  -out "$CERT_DIR/client-cert.pem"

# Clean up temporary files
rm -f "$CERT_DIR/server.csr" "$CERT_DIR/client.csr" "$CERT_DIR/server-ext.cnf"

echo "Certificates generated successfully!"
echo ""
echo "Server certificates:"
echo "  - Certificate: $CERT_DIR/server-cert.pem"
echo "  - Private key: $CERT_DIR/server-key.pem"
echo ""
echo "Client certificates (for mTLS):"
echo "  - Certificate: $CERT_DIR/client-cert.pem"
echo "  - Private key: $CERT_DIR/client-key.pem"
echo ""
echo "CA certificate:"
echo "  - Certificate: $CERT_DIR/ca-cert.pem"
