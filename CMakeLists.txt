cmake_minimum_required(VERSION 3.16)
project(etcd_mvp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ETCD_MVP_BUILD_TESTS "Build unit tests" ON)
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_FILE ${PROTO_SRC_DIR}/etcd_mvp.proto)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILE})

get_target_property(GRPC_CPP_PLUGIN_LOCATION gRPC::grpc_cpp_plugin LOCATION)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/etcd_mvp.grpc.pb.cc ${CMAKE_CURRENT_BINARY_DIR}/etcd_mvp.grpc.pb.h
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
  ARGS --grpc_out ${CMAKE_CURRENT_BINARY_DIR}
       --cpp_out ${CMAKE_CURRENT_BINARY_DIR}
       -I ${PROTO_SRC_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_LOCATION}
       ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
)

add_library(etcdmvp
  src/raft.cpp
  src/raft_grpc_transport.cpp
  src/storage_wal.cpp
  src/storage_snapshot.cpp
  src/storage_config.cpp
  src/lease_manager.cpp
  src/kv_engine.cpp
  src/watch_manager.cpp
  src/cluster.cpp
  src/node.cpp
  src/grpc_server.cpp
  ${PROTO_SRCS}
  ${CMAKE_CURRENT_BINARY_DIR}/etcd_mvp.grpc.pb.cc
)

target_include_directories(etcdmvp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
  ${PROTO_SRC_DIR}
)

target_link_libraries(etcdmvp PUBLIC
  gRPC::grpc++
  protobuf::libprotobuf
)

add_executable(etcd_mvp
  src/main.cpp
)

target_link_libraries(etcd_mvp PRIVATE etcdmvp)

add_executable(etcd_mvp_client
  examples/grpc_client.cpp
)

target_link_libraries(etcd_mvp_client PRIVATE etcdmvp)

if(ETCD_MVP_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/release-1.12.1.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(etcdmvp_tests
    tests/test_wal.cpp
    tests/test_snapshot.cpp
    tests/test_watch_manager.cpp
    tests/test_kv_engine.cpp
    tests/test_lease_manager.cpp
    tests/test_raft.cpp
  )

  target_include_directories(etcdmvp_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_link_libraries(etcdmvp_tests PRIVATE
    etcdmvp
    GTest::gtest_main
  )

  include(GoogleTest)
  gtest_discover_tests(etcdmvp_tests)
endif()
