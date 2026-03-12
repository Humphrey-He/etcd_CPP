# C++ 版本 etcd MVP（SDD 规格驱动开发）文档

本文基于 upstream etcd 的公开文档与仓库信息，整理核心功能、亮点与应用场景，并给出面向 C++ 实现的 MVP 规格与设计方案。
上游仓库：`https://github.com/etcd-io/etcd`

---

## 1. etcd 核心功能与亮点（检索摘要）

- 强一致性分布式键值存储：etcd 是强一致性的分布式 KV 存储，面向分布式系统关键数据。citeturn1search2
- Raft 一致性协议：etcd 通过 Raft 协议实现可靠分布式一致性与 leader 选举。citeturn1search2
- gRPC API：etcd 提供 gRPC API；v3 API 定义了 `KV` 与 `Watch` 服务。citeturn1search4turn0search0
- Watch 机制：Watch API 以事件形式监控 key 变更，事件类型包含 PUT/DELETE。citeturn1search0
- WAL 持久化：WAL 保存 Raft 提议与状态（包含 hard-state 与轻量快照），并以追加写方式持久化。citeturn0search1

---

## 2. 主要应用场景（检索摘要）

- Kubernetes 控制平面主数据存储：etcd 被 Kubernetes 作为主要数据存储与后端一致性存储。citeturn1search5turn1search1
- 服务发现与配置管理：etcd 的 Watch 能提供变更通知，适合作为一致性配置存储与通知中心。citeturn1search2turn1search0
- 分布式协调与元数据：强一致 KV + Raft 适合分布式系统元数据与协调信息存储。citeturn1search2

---

## 3. SDD：规格驱动设计（MVP 范围）

### 3.1 设计目标（必须满足）
- 强一致性：所有写操作通过 Raft 达成共识并提交后才返回成功。
- 可恢复性：节点可通过 WAL + 快照进行恢复。citeturn0search1
- API 兼容性（子集）：提供与 etcd v3 语义一致的 gRPC 子集接口（Put/Get/Delete/Watch/MemberList/Status）。citeturn0search0turn0search2
- 可观测性：基础日志与指标。
- 可测试性：单元测试与 3 节点集成测试。

### 3.2 非功能性约束
- 语言与标准：C++17。
- 平台优先：Linux 优先，兼容 Windows 作为开发环境。
- 性能目标（MVP）：单节点 2,000 writes/s（可作为初期指标，不达标需说明差距）。
- 安全：TLS 可选，认证与 RBAC 后续迭代。
- 集群规模：固定 3 节点静态集群。

### 3.3 模块划分与对外契约

| 模块 | 职责 | 对外契约 |
|---|---:|---|
| Raft Core | 日志复制、leader 选举、心跳、投票 | AppendEntries/RequestVote RPC；提供 Apply(index, entry) 回调 |
| Storage | WAL、快照、状态机持久化 | AppendLog(entry)、ReadLog(range)、CreateSnapshot()、LoadSnapshot() |
| KV Engine | 状态机应用日志 | ApplyCommand(cmd)、Get(key)、List(prefix) |
| gRPC API Server | 处理客户端请求 | Put/Get/Delete/Watch/MemberList/Status |
| Watch Manager | 订阅与事件分发 | WatchRegister(key/prefix, startRevision) -> Stream |
| Cluster Manager | 成员管理 | AddMember/RemoveMember、MemberList |
| Metrics & Logging | 指标与日志 | Prometheus / JSON 日志 |
| Test Harness | 故障注入与集群模拟 | 网络分区/延迟/节点重启 |

---

## 4. 规格与接口（MVP API 子集）

etcd API 以 gRPC 服务形式提供。citeturn1search4
MVP 采用 v3 语义子集接口：

```proto
service KV {
  rpc Put(PutRequest) returns (PutResponse);
  rpc Get(RangeRequest) returns (RangeResponse);
  rpc DeleteRange(DeleteRangeRequest) returns (DeleteRangeResponse);
}

service Watch {
  rpc Watch(WatchRequest) returns (stream WatchResponse);
}

service Cluster {
  rpc MemberList(Empty) returns (MemberListResponse);
  rpc Status(Empty) returns (StatusResponse);
}
```

Put/Delete 会增加 revision，并在事件历史中生成事件。citeturn0search2
历史修订在 compaction 后不可访问，需在可用历史窗口内订阅。citeturn0search5

---

## 5. 核心实现策略（SDD 驱动）

### 5.1 Raft Core（MVP 必要语义）
- Leader 选举与心跳：随机化选举超时，避免票冲突。
- 日志复制：AppendEntries 进行 log replication，follower 持久化 WAL 后应答。citeturn0search1
- Commit 流程：leader 过半确认后推进 commitIndex，并调用 Apply。

### 5.2 存储层
- WAL：每条 entry 含 term、index、payload、checksum。
- WAL 追加写入，包含 hard-state 与轻量快照信息。citeturn0search1
- Snapshot：定期生成快照，包含完整 KV 状态与 lastApplied。
- 恢复流程：先加载快照，再回放 WAL 到最新一致状态。citeturn0search1

### 5.3 KV 状态机
- ApplyCommand 串行执行，确保与 Raft commit 顺序一致。
- 读路径：MVP 阶段仅允许 leader 处理读，保障线性一致。

### 5.4 Watch 机制
- WatchManager 维护 watcher 列表（key/prefix -> subscriber）。
- Apply 时触发事件推送。
- 若 watch 指定的 revision 已被 compact，服务端将返回 compact_revision 并取消 watch。citeturn0search0

### 5.5 gRPC 服务实现
- KV/Watch 以 gRPC 实现，与 etcd v3 语义对齐。citeturn0search0turn0search2
- Leader 转移：非 leader 节点返回 NotLeader，并提供 leader hint。

---

## 6. 关键数据结构（建议）

```cpp
struct LogEntry {
  uint64_t index;
  uint64_t term;
  std::string data;
  uint32_t crc32;
};

struct HardState {
  uint64_t current_term;
  uint64_t voted_for;
  uint64_t commit_index;
};

struct SnapshotMeta {
  uint64_t last_index;
  uint64_t last_term;
};
```

---

## 7. 测试与验证

- 单元测试：Raft state transition、log match、WAL replay、KV apply。
- 集成测试：3 节点本地集群，验证 leader 选举与写一致性。
- 故障注入：网络延迟、leader 挂掉、follower 重启。

---

## 8. 开发计划（里程碑）

| 里程碑 | 交付物 | 预计工期（人日） |
|---|---:|---:|
| 1. 需求与 SDD 完整文档 | 本文档 + Proto 定义 | 2 |
| 2. 基础模块骨架 | 项目脚手架、接口与 CMake | 3 |
| 3. Storage + WAL + Snapshot | 持久化与恢复流程 | 5 |
| 4. Raft Core | 选举、日志复制、RPC | 10 |
| 5. KV Engine | 状态机与基本 KV API | 5 |
| 6. gRPC Server + Watch | API 实现与事件流 | 6 |
| 7. 集成与 3 节点测试 | 自动化集成测试 | 6 |
| 8. 文档与示例 | README、部署与指标 | 3 |
| 合计 | MVP 发布 | 40 |

---

## 9. 后续迭代方向

- 引入 RocksDB/LMDB 提升存储性能。
- 完整实现 joint consensus 与动态成员变更。
- 读性能优化（ReadIndex / lease-based reads）。
- TLS 自动证书管理与 RBAC。
