#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DATA_DIR="$PROJECT_ROOT/test_data"

cleanup() {
  echo "Cleaning up..."
  pkill -f etcd_mvp || true
  rm -rf "$DATA_DIR"
  exit
}

trap cleanup EXIT INT TERM

echo "Building project..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

echo "Cleaning test data..."
rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"

echo "Starting 3-node cluster..."
export ETCD_MVP_PEERS="1=127.0.0.1:2379,2=127.0.0.1:2380,3=127.0.0.1:2381"

ETCD_MVP_NODE_ID=1 ETCD_MVP_LISTEN="0.0.0.0:2379" ETCD_MVP_DATA_DIR="$DATA_DIR/node1" ./Release/etcd_mvp &
NODE1_PID=$!

ETCD_MVP_NODE_ID=2 ETCD_MVP_LISTEN="0.0.0.0:2380" ETCD_MVP_DATA_DIR="$DATA_DIR/node2" ./Release/etcd_mvp &
NODE2_PID=$!

ETCD_MVP_NODE_ID=3 ETCD_MVP_LISTEN="0.0.0.0:2381" ETCD_MVP_DATA_DIR="$DATA_DIR/node3" ./Release/etcd_mvp &
NODE3_PID=$!

echo "Waiting for cluster to start..."
sleep 5

echo "Running integration tests..."
cd "$PROJECT_ROOT/tests/integration"
./integration_tests

echo "All tests passed!"
