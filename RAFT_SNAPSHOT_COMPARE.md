# C++ etcd MVP：Raft 状态机与日志压缩（Snapshot）功能说明与对比分析

本文分两部分说明：
1. 本项目的 Raft 状态机与日志压缩（Snapshot/WAL）实现方式
2. 与原生 etcd 的实现做对比分析

## 1. 本项目实现说明

### 1.1 Raft 状态机实现（C++ MVP）

**核心结构**
- RaftNode 内部维护 `HardState`（current_term/voted_for/commit_index）、日志 `LogEntry` 列表、`next_index/match_index`、角色（Follower/Candidate/Leader）。
- 选举与心跳在独立线程中定时 `Tick`，leader 定期发送 AppendEntries 心跳。 
- 通过 gRPC 进行跨进程 RPC：`AppendEntries` 与 `RequestVote`。

**主流程**
1. `Tick`：累积心跳/选举计时；超时进入 Candidate 选举。
2. `BecomeCandidate`：自增 term、给自己投票、向 peers 发起 RequestVote；获得多数即成为 leader。
3. `BecomeLeader`：初始化每个 follower 的 `next_index/match_index`。
4. `SendHeartbeats`：发送 AppendEntries；根据响应回退或推进 `next_index`。
5. `AdvanceCommitIndex`：多数确认后更新 `commit_index`。
6. `ApplyCommitted`：将已提交的日志按序回调状态机（KV/Lease/Watch）。

**当前约束**
- 读写均由 leader 处理（leader-only）。
- 未实现 joint consensus、dynamic membership、ReadIndex/Lease Read、PreVote、线性一致读优化。
- 日志复制为“全量追赶 + 逐条回退”方式，未做 pipeline 优化。

### 1.2 日志与快照（WAL + Snapshot）

**WAL 记录格式**
```
term:uint64 | index:uint64 | type:uint8 | length:uint32 | checksum:uint32 | payload
```
- checksum 为 payload 的 CRC32。
- WAL 轮转：默认 64MB（`ETCD_MVP_WAL_SEGMENT_SIZE`）。
- 可配置 fsync（`ETCD_MVP_FSYNC`）。

**快照触发与写入**
- 以 apply revision 计数触发（`ETCD_MVP_SNAPSHOT_THRESHOLD`）。
- 生成临时文件，写入完整 KV 镜像 + 元数据后原子重命名。
- 快照成功后清理 `apply_index` 之前的 WAL 段。

**恢复流程**
1. 启动先加载最新快照（若存在）。
2. 读取 WAL 段，回放 index > snapshot.apply_index 的日志条目。
3. 校验 checksum，发现损坏即返回恢复错误码。

## 2. 与原生 etcd 的对比分析

### 2.1 Raft 状态机实现范式对比

**本项目（C++ MVP）**
- Raft 与存储、网络一体化封装；日志、状态机应用逻辑直接内嵌在 RaftNode 中。
- RPC 仅实现 AppendEntries/RequestVote，两阶段交互简化。

**原生 etcd**
- etcd 使用 `etcd-io/raft` 作为独立 Raft 库，该库仅实现 Raft 核心算法，网络与磁盘 IO 由上层实现。citeturn0search1
- 该库通过 Ready/Advance 机制让应用层负责持久化、消息发送与状态机应用，从而实现分层解耦。citeturn1search0

**结论**
- MVP 的 Raft 实现更直接，便于理解与演示，但缺少原生 etcd 的模块化、可扩展性与优化路径。citeturn0search1

### 2.2 日志压缩与快照机制对比

**本项目（C++ MVP）**
- 以 apply revision 达到阈值时生成快照。
- 快照完成后，清理快照 index 之前的 WAL segments。
- WAL 只保存日志条目，快照保存 KV 的全量镜像与必要元数据。

**原生 etcd**
- 通过 `--snapshot-count` 控制保留的 Raft 日志数量；达到阈值后生成快照并截断旧日志。citeturn0search6
- 较高的 snapshot-count 会保留更多日志以帮助慢 follower 追赶，较低则更频繁快照。citeturn0search6
- etcd 同时提供 MVCC 历史 compaction（按 revision 清理历史版本），避免历史无限增长。citeturn0search0

**结论**
- 本项目实现了 Raft snapshot 与 WAL 清理，但缺少 etcd 的慢 follower 追赶策略与 MVCC 历史 compaction 机制。citeturn0search6turn0search0

## 3. 功能差异清单（摘要）

| 维度 | 本项目 C++ MVP | 原生 etcd |
|---|---|---|
| Raft 结构 | 单体 RaftNode + 直接 IO | etcd-io/raft + Ready/Advance 解耦 citeturn0search1turn1search0 |
| 日志压缩 | 阈值触发快照 + WAL 清理 | snapshot-count 控制 + 慢 follower 追赶 citeturn0search6 |
| MVCC compaction | 未实现 | 支持历史 compaction citeturn0search0 |
| 可扩展性 | 简化实现 | 分层设计、扩展性强 citeturn0search1 |

## 4. 建议的后续增强方向

1. 引入 Ready/Advance 模式，将 Raft 核心与 IO 解耦。citeturn1search0
2. 增加慢 follower 追赶策略（快照传输与恢复）。citeturn0search6
3. 增加 MVCC compaction（按 revision 清理历史版本）。citeturn0search0


