# etcd C++ MVP

这是一个基于 SDD 范畴实现的 C++ etcd MVP，包含：
- Raft 日志复制与 leader 选举（跨进程 gRPC RPC）
- gRPC API（Put/Get/Delete/Watch/Txn/Lease）
- MVCC（多版本元信息：create/mod/version/lease）
- Watch 历史缓存与 compaction（按容量裁剪）
- WAL 记录格式（带 header + checksum）
- WAL 轮转、fsync 可配置
- 快照生成、原子写入、WAL 清理
- 恢复流程：快照加载 + WAL 回放

## 构建

需要本地安装 gRPC 与 Protobuf（vcpkg/系统包皆可）。

```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## 运行（跨进程 Raft）

准备 peers 列表：
```
ETCD_MVP_PEERS=1=127.0.0.1:2379,2=127.0.0.1:2380,3=127.0.0.1:2381
```

启动 3 个终端：
```powershell
# node1
$env:ETCD_MVP_NODE_ID=1
$env:ETCD_MVP_LISTEN="0.0.0.0:2379"
$env:ETCD_MVP_PEERS="1=127.0.0.1:2379,2=127.0.0.1:2380,3=127.0.0.1:2381"
.\Release\etcd_mvp.exe

# node2
$env:ETCD_MVP_NODE_ID=2
$env:ETCD_MVP_LISTEN="0.0.0.0:2380"
$env:ETCD_MVP_PEERS="1=127.0.0.1:2379,2=127.0.0.1:2380,3=127.0.0.1:2381"
.\Release\etcd_mvp.exe

# node3
$env:ETCD_MVP_NODE_ID=3
$env:ETCD_MVP_LISTEN="0.0.0.0:2381"
$env:ETCD_MVP_PEERS="1=127.0.0.1:2379,2=127.0.0.1:2380,3=127.0.0.1:2381"
.\Release\etcd_mvp.exe
```

## gRPC 客户端示例

```powershell
# 写入
.\Release\etcd_mvp_client.exe 127.0.0.1:2379 put foo bar

# 读取
.\Release\etcd_mvp_client.exe 127.0.0.1:2379 get foo

# 删除
.\Release\etcd_mvp_client.exe 127.0.0.1:2379 del foo

# Watch 前缀
.\Release\etcd_mvp_client.exe 127.0.0.1:2379 watch foo/

# 租约
.\Release\etcd_mvp_client.exe 127.0.0.1:2379 lease-grant 10
.\Release\etcd_mvp_client.exe 127.0.0.1:2379 put foo bar <lease_id>
```

## 配置项（环境变量）

- `ETCD_MVP_NODE_ID`：节点 ID
- `ETCD_MVP_LISTEN`：本节点监听地址
- `ETCD_MVP_PEERS`：集群 peers 列表（id=addr 逗号分隔）
- `ETCD_MVP_DATA_DIR`：数据目录根路径，默认 `data/node{ID}`
- `ETCD_MVP_WAL_SEGMENT_SIZE`：WAL 分段大小（字节），默认 64MB
- `ETCD_MVP_SNAPSHOT_THRESHOLD`：触发快照的 apply 条数阈值，默认 1000
- `ETCD_MVP_WATCH_HISTORY_SIZE`：watch 历史缓存大小（事件条数），默认 10000
- `ETCD_MVP_FSYNC`：是否启用 fsync（1/0 或 true/false），默认 true
- `ETCD_MVP_RPC_TIMEOUT_S`：RPC 超时秒数，默认 5

## WAL 记录格式（MVP）

每条记录：
```
term:uint64 | index:uint64 | type:uint8 | length:uint32 | checksum:uint32 | payload
```
- `checksum` 为 payload 的 CRC32
- `type=1` 表示 Raft entry

## 快照格式（MVP）

```
apply_index:uint64 | last_term:uint64 | kv_count:uint64 | (klen,vlen,key,val)* | metadata_len:uint64 | metadata_json
```

快照写入采用临时文件 + 原子重命名，完成后清理 apply_index 之前的 WAL segment。

## 恢复流程

启动时：
1. 加载最新快照（如存在）
2. 从 WAL 回放 apply_index 之后的日志
3. 校验 checksum，遇损坏则返回错误并停止恢复

## 恢复错误码与建议

- `SNAPSHOT_NOT_FOUND`：无快照，正常现象，可忽略
- `SNAPSHOT_CORRUPT`：快照损坏，建议恢复备份或删除快照并重新拉起集群
- `SNAPSHOT_IO_ERROR`：磁盘写入/读取失败，检查磁盘权限与空间
- `WAL_CORRUPT`：WAL 损坏，建议回滚到最近快照并清理损坏 WAL
- `WAL_IO_ERROR`：磁盘写入/读取失败，检查磁盘权限与空间

## Watch 历史与 compaction

- Watch 历史采用固定大小缓存（`ETCD_MVP_WATCH_HISTORY_SIZE`）
- `start_revision` 小于历史最早 revision 时返回 `HISTORY_UNAVAILABLE` 并给出 `compact_revision`

## MVCC/Txn/Lease（MVP）

- 每次写入递增 revision，并提供 `create_revision/mod_revision/version/lease`
- Txn 支持简单 compare + success/failure 分支
- Lease 支持 grant/revoke/keepalive，租约到期会自动删除关联 key

## 验证流程（建议）

1. 启动 3 节点
2. 使用客户端 Put/Get/Del 验证读写与 leader redirect
3. 启动 Watch，执行 Put/Del 验证事件流
4. 重启节点，确认数据恢复（快照 + WAL 回放）
