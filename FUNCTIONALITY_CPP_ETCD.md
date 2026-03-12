# C++ etcd MVP 功能说明（实现程度检查）

本文基于当前代码实现进行功能核对，描述已实现能力、行为契约、配置与限制。

## 1. 当前实现概览

- 运行形态：单进程一个节点，3 节点跨进程集群，通过 gRPC 进行 Raft RPC 通信
- API：gRPC（KV/Watch/Txn/Lease/Cluster/Raft）
- 一致性：Leader-only 读写，写入经 Raft 提交后应用
- 持久化：WAL（header + checksum）+ 快照 + 启动恢复 + WAL 轮转 + fsync 可配置
- Watch：在线订阅 + 历史事件缓存 + compaction
- MVCC：单 key 版本链（create/mod/version/lease）

## 2. 功能实现清单

| 功能 | 实现状态 | 备注 |
|---|---|---|
| Raft 选举与日志复制 | 已实现 | leader 选举 + AppendEntries/RequestVote，跨进程 gRPC RPC |
| 强一致写入 | 已实现 | 仅 leader 接收写请求，Raft 提交后 apply |
| 强一致读取 | 已实现 | 仅 leader 接收读请求（MVP 简化） |
| KV Put/Get/Delete | 已实现 | gRPC KV 服务 |
| Watch | 已实现 | 支持 prefix，历史回放 + 在线推送 |
| Watch 历史持久化 | 已实现 | 快照时持久化，启动加载 |
| Watch compaction | 已实现 | 基于历史缓存上限裁剪 |
| WAL 记录格式 | 已实现 | header + checksum + payload |
| WAL 轮转 | 已实现 | 默认 64MB，可配置 |
| fsync | 已实现 | WAL/快照可配置 |
| 快照生成 | 已实现 | temp file + 原子重命名 |
| 恢复流程 | 已实现 | 先快照，后 WAL 回放并校验 checksum |
| MVCC 元信息 | 已实现 | create/mod/version/lease |
| Txn 原子化 | 已实现 | 单条 Raft entry 编码事务 |
| Lease | 已实现 | grant/revoke/keepalive，超时删除 key |
| Cluster 状态 | 已实现 | MemberList/Status |
| TLS/认证 | 未实现 | 预留后续迭代 |
| 动态成员变更 | 未实现 | 静态 peers |
| 读索引/lease read | 未实现 | 后续优化 |

## 3. 关键行为契约

### 3.1 写入路径（Put/Delete/Txn/Lease）

1. 客户端请求进入 gRPC KV/Lease 服务
2. 若本节点非 leader，返回 NOT_LEADER + leader_address
3. leader 将请求编码为 Raft entry，Append 到 WAL
4. 通过 Raft AppendEntries 广播
5. 达成多数提交后 apply 到 KV/Lease/Watch

### 3.2 读取路径（Get/Txn Compare）

- 仅 leader 处理读请求
- MVCC 元信息随 KeyValue 返回（create/mod/version/lease）

### 3.3 Watch

- 注册时按 start_revision 回放历史
- start_revision 早于历史最小 revision 返回 HISTORY_UNAVAILABLE 并给出 compact_revision
- 之后持续推送新事件

### 3.4 Txn

- Compare 在 leader 本地执行
- success/failure 分支的写操作编码为单条 Raft entry，实现原子 apply

### 3.5 Lease

- Grant/KeepAlive/Revoke 通过 Raft 复制
- Lease 到期在 leader 侧触发删除 key，删除操作也通过 Raft 复制

## 4. 存储与恢复

### 4.1 WAL 记录格式

```
term:uint64 | index:uint64 | type:uint8 | length:uint32 | checksum:uint32 | payload
```

- checksum 为 payload 的 CRC32
- 轮转阈值默认 64MB，可配置

### 4.2 快照格式

```
apply_index:uint64 | last_term:uint64 | kv_count:uint64 | (klen,vlen,key,val)* | metadata_len:uint64 | metadata_json
```

### 4.3 恢复流程

1. 加载最新快照
2. 回放 WAL 中 index > snap.apply_index 的条目
3. 校验 checksum，遇损坏返回错误并停止恢复

## 5. 配置项

- `ETCD_MVP_NODE_ID`
- `ETCD_MVP_LISTEN`
- `ETCD_MVP_PEERS`
- `ETCD_MVP_DATA_DIR`
- `ETCD_MVP_WAL_SEGMENT_SIZE`
- `ETCD_MVP_SNAPSHOT_THRESHOLD`
- `ETCD_MVP_WATCH_HISTORY_SIZE`
- `ETCD_MVP_FSYNC`
- `ETCD_MVP_RPC_TIMEOUT_S`

## 6. 已知限制与待修复

- ErrorCode 枚举中缺少 `NOT_FOUND`，但代码在 LeaseKeepAlive 里会返回该码（需要补充枚举）。
- Txn compare 目前未做更严格的线性化读索引校验（MVP 简化为 leader 本地读）。
- Watch 历史基于内存队列，持久化在快照时刷新，崩溃后可能丢失快照后到崩溃前的历史。
- 未实现 TLS、认证、动态成员变更、读性能优化与压缩。

## 7. 本地验证建议

1. 启动 3 节点并确认 leader
2. 在 leader 上执行 Put/Get/Delete
3. 开启 Watch，执行写入验证事件
4. 执行 Lease grant + put with lease，等待过期后验证删除
5. 重启节点验证快照 + WAL 恢复
