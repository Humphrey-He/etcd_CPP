# C++ etcd MVP 下一期开发任务清单

生成时间：2026-04-20  
当前版本：MVP v1.0（commit 0db79af）

## 项目进度总结

### 已完成（MVP v1.0）
- ✅ Raft 一致性协议（选举、日志复制、提交）
- ✅ 跨进程 gRPC RPC 通信
- ✅ WAL 持久化（分段、轮转、checksum）
- ✅ 快照生成与恢复
- ✅ KV 引擎（MVCC 元信息）
- ✅ Watch 机制（历史回放、compaction）
- ✅ Lease 管理（grant/revoke/keepalive/过期）
- ✅ gRPC API（KV/Watch/Lease/Cluster/Raft）
- ✅ 单元测试（6 个测试文件，427 行）
- ✅ 完整文档（SDD、设计、功能、对比、状态报告）

### 当前限制
- ❌ 无集成测试（3 节点集群端到端验证）
- ❌ 无性能基准测试
- ❌ 日志输出不完善（缺少结构化日志）
- ❌ 无监控指标（Prometheus）
- ❌ 错误处理不完善（部分场景未覆盖）
- ❌ 无 TLS 加密通信
- ❌ 无认证机制

---

## 第二期开发任务（v1.1）

**目标**：提升稳定性、可测试性、可观测性  
**预计工期**：15-20 人日  
**优先级**：P0（必须完成）

### 任务 1：集成测试框架（5 人日）

**目标**：验证 3 节点集群的端到端功能

#### 1.1 测试脚本基础设施
- [ ] 创建 `scripts/integration_test.sh`
  - 自动启动 3 节点集群
  - 等待 leader 选举完成
  - 执行测试用例
  - 清理测试环境
- [ ] 创建 `tests/integration/` 目录
- [ ] 添加测试辅助函数（等待 leader、检查集群状态）

#### 1.2 核心功能集成测试
- [ ] **测试用例 1**：Leader 选举
  - 启动 3 节点，验证选出 leader
  - 验证 follower 正确识别 leader
- [ ] **测试用例 2**：写入一致性
  - 在 leader 写入数据
  - 验证所有节点数据一致
- [ ] **测试用例 3**：读取一致性
  - 写入后从 leader 读取
  - 验证返回正确数据
- [ ] **测试用例 4**：Watch 事件流
  - 注册 watch
  - 执行 put/delete
  - 验证事件正确推送
- [ ] **测试用例 5**：Lease 过期
  - 创建 lease 并绑定 key
  - 等待过期
  - 验证 key 被自动删除
- [ ] **测试用例 6**：节点重启恢复
  - 写入数据
  - 重启一个节点
  - 验证数据恢复（快照 + WAL）
- [ ] **测试用例 7**：Leader 故障转移
  - 杀掉 leader 进程
  - 验证新 leader 选举
  - 验证数据仍可访问

#### 1.3 CI 集成
- [ ] 添加 GitHub Actions 配置（`.github/workflows/integration.yml`）
- [ ] 在 CI 中运行集成测试
- [ ] 添加测试覆盖率报告

**交付物**：
- `scripts/integration_test.sh`
- `tests/integration/*.cpp`（7+ 测试用例）
- `.github/workflows/integration.yml`

---

### 任务 2：性能基准测试（3 人日）

**目标**：建立性能基线，识别瓶颈

#### 2.1 基准测试工具
- [ ] 创建 `tools/benchmark.cpp`
  - 支持配置并发数、请求数、key 大小、value 大小
  - 输出 QPS、延迟分布（p50/p95/p99）、吞吐量
- [ ] 支持多种测试模式
  - 纯写入（Put）
  - 纯读取（Get）
  - 混合读写（80% read, 20% write）
  - Watch 订阅性能

#### 2.2 基准测试场景
- [ ] **场景 1**：单节点写入性能
  - 目标：2,000 writes/s（SDD 目标）
- [ ] **场景 2**：单节点读取性能
- [ ] **场景 3**：3 节点集群写入性能
- [ ] **场景 4**：Watch 事件延迟
- [ ] **场景 5**：大 value 写入性能（1KB、10KB、100KB）

#### 2.3 性能报告
- [ ] 生成性能基线报告（`docs/PERFORMANCE_BASELINE.md`）
- [ ] 识别性能瓶颈（CPU、网络、磁盘 I/O）
- [ ] 提出优化建议

**交付物**：
- `tools/benchmark.cpp`
- `docs/PERFORMANCE_BASELINE.md`

---

### 任务 3：可观测性增强（4 人日）

**目标**：完善日志、添加指标导出

#### 3.1 结构化日志
- [ ] 统一日志格式（JSON）
  - 时间戳、级别、模块、消息、trace_id
- [ ] 添加日志级别控制（环境变量 `ETCD_MVP_LOG_LEVEL`）
- [ ] 关键路径日志
  - Raft 状态转换（Follower → Candidate → Leader）
  - 日志复制（AppendEntries 成功/失败）
  - 快照生成与恢复
  - WAL 轮转
  - Lease 过期

#### 3.2 Prometheus 指标
- [ ] 添加 Prometheus C++ 客户端依赖
- [ ] 实现指标导出 HTTP 端点（`/metrics`）
- [ ] 核心指标
  - `etcd_raft_role`：当前角色（0=Follower, 1=Candidate, 2=Leader）
  - `etcd_raft_term`：当前 term
  - `etcd_raft_commit_index`：已提交索引
  - `etcd_raft_applied_index`：已应用索引
  - `etcd_kv_revision`：当前 revision
  - `etcd_kv_total_keys`：总 key 数量
  - `etcd_watch_watchers`：活跃 watcher 数量
  - `etcd_lease_total`：活跃 lease 数量
  - `etcd_wal_fsync_duration_seconds`：WAL fsync 延迟
  - `etcd_snapshot_save_duration_seconds`：快照保存延迟
  - `etcd_grpc_requests_total`：gRPC 请求计数（按方法、状态码）
  - `etcd_grpc_request_duration_seconds`：gRPC 请求延迟

#### 3.3 监控示例
- [ ] 提供 Grafana dashboard JSON（`docs/grafana_dashboard.json`）
- [ ] 提供 Prometheus 配置示例（`docs/prometheus.yml`）

**交付物**：
- 结构化日志输出
- `/metrics` 端点（12+ 指标）
- `docs/grafana_dashboard.json`
- `docs/prometheus.yml`

---

### 任务 4：错误处理与稳定性（3 人日）

**目标**：完善错误处理，提升鲁棒性

#### 4.1 错误处理增强
- [ ] 统一错误码定义（扩展 proto ErrorCode）
  - `TIMEOUT`：RPC 超时
  - `UNAVAILABLE`：服务不可用
  - `INVALID_ARGUMENT`：参数错误
  - `RESOURCE_EXHAUSTED`：资源耗尽
- [ ] 完善错误传播
  - Raft 层错误 → gRPC 层错误码映射
  - 存储层错误 → 恢复错误码映射
- [ ] 添加重试逻辑
  - gRPC 客户端自动重试（指数退避）
  - Raft RPC 重试（AppendEntries/RequestVote）

#### 4.2 边界条件处理
- [ ] 空 key/value 处理
- [ ] 超大 value 限制（默认 1MB）
- [ ] 并发写入冲突处理
- [ ] 磁盘空间不足处理
- [ ] 网络分区恢复

#### 4.3 资源限制
- [ ] 添加配置项
  - `ETCD_MVP_MAX_REQUEST_SIZE`：最大请求大小
  - `ETCD_MVP_MAX_CONCURRENT_STREAMS`：最大并发流
  - `ETCD_MVP_MAX_WATCHERS`：最大 watcher 数量
- [ ] 实现资源限制检查

**交付物**：
- 扩展的 ErrorCode 枚举
- 完善的错误处理逻辑
- 资源限制配置

---

### 任务 5：文档与示例（2 人日）

**目标**：完善使用文档和示例

#### 5.1 运维文档
- [ ] 创建 `docs/OPERATIONS.md`
  - 部署指南（单机、集群）
  - 配置参数详解
  - 监控指标说明
  - 故障排查手册
  - 备份与恢复流程

#### 5.2 API 使用示例
- [ ] 扩展 `examples/grpc_client.cpp`
  - 添加 Txn 示例
  - 添加 Watch 取消示例
  - 添加错误处理示例
- [ ] 创建 Python 客户端示例（`examples/python_client.py`）
- [ ] 创建 Go 客户端示例（`examples/go_client.go`）

#### 5.3 性能调优指南
- [ ] 创建 `docs/PERFORMANCE_TUNING.md`
  - WAL 配置优化
  - 快照阈值调优
  - Watch 历史大小调优
  - 网络参数调优

**交付物**：
- `docs/OPERATIONS.md`
- `docs/PERFORMANCE_TUNING.md`
- 多语言客户端示例

---

## 第三期开发任务（v1.2）

**目标**：安全性、性能优化  
**预计工期**：20-25 人日  
**优先级**：P1（重要）

### 任务 6：TLS 加密通信（6 人日）
- [ ] gRPC TLS 配置
- [ ] 证书管理（生成、轮转）
- [ ] mTLS 双向认证
- [ ] 配置项：`ETCD_MVP_TLS_CERT`、`ETCD_MVP_TLS_KEY`、`ETCD_MVP_TLS_CA`

### 任务 7：基础认证机制（5 人日）
- [ ] 用户管理（添加、删除、修改密码）
- [ ] 基于 token 的认证
- [ ] 权限控制（读/写权限）
- [ ] 配置项：`ETCD_MVP_AUTH_ENABLED`

### 任务 8：ReadIndex 优化（4 人日）
- [ ] 实现 ReadIndex 协议
- [ ] Follower 读取支持
- [ ] 线性一致性读取验证

### 任务 9：慢 Follower 快照传输（5 人日）
- [ ] 检测慢 follower（next_index 落后过多）
- [ ] 快照传输 RPC
- [ ] 快照接收与应用
- [ ] 传输进度监控

### 任务 10：性能优化（5 人日）
- [ ] Pipeline 日志复制
- [ ] Batch 写入优化
- [ ] 内存池优化
- [ ] 零拷贝优化

---

## 第四期开发任务（v2.0）

**目标**：生产级特性  
**预计工期**：30-40 人日  
**优先级**：P2（长期）

### 任务 11：Ready/Advance 模式重构（10 人日）
- [ ] 解耦 Raft 核心与 IO
- [ ] 实现 Ready/Advance 接口
- [ ] 迁移现有代码

### 任务 12：动态成员变更（8 人日）
- [ ] Joint Consensus 实现
- [ ] AddMember/RemoveMember API
- [ ] 配置变更日志

### 任务 13：RocksDB 存储引擎（10 人日）
- [ ] 集成 RocksDB
- [ ] KV 引擎迁移
- [ ] 性能对比测试

### 任务 14：MVCC Compaction（7 人日）
- [ ] 历史版本清理
- [ ] Compaction API
- [ ] 自动 compaction 策略

---

## 任务优先级矩阵

| 任务 | 优先级 | 复杂度 | 价值 | 依赖 |
|-----|-------|-------|-----|-----|
| 集成测试框架 | P0 | 中 | 高 | 无 |
| 性能基准测试 | P0 | 低 | 高 | 无 |
| 可观测性增强 | P0 | 中 | 高 | 无 |
| 错误处理与稳定性 | P0 | 中 | 高 | 无 |
| 文档与示例 | P0 | 低 | 中 | 无 |
| TLS 加密通信 | P1 | 中 | 高 | 无 |
| 基础认证机制 | P1 | 中 | 高 | TLS |
| ReadIndex 优化 | P1 | 高 | 中 | 无 |
| 慢 Follower 快照传输 | P1 | 高 | 中 | 无 |
| 性能优化 | P1 | 高 | 高 | 基准测试 |
| Ready/Advance 重构 | P2 | 高 | 中 | 无 |
| 动态成员变更 | P2 | 高 | 中 | Ready/Advance |
| RocksDB 存储引擎 | P2 | 高 | 高 | 无 |
| MVCC Compaction | P2 | 中 | 中 | 无 |

---

## 建议开发顺序

### 第二期（v1.1）- 立即开始
1. **Week 1-2**：集成测试框架 + 性能基准测试
2. **Week 3**：可观测性增强
3. **Week 4**：错误处理与稳定性 + 文档

### 第三期（v1.2）- 2 个月后
1. **Week 1-2**：TLS 加密通信 + 基础认证
2. **Week 3**：ReadIndex 优化
3. **Week 4**：慢 Follower 快照传输
4. **Week 5**：性能优化

### 第四期（v2.0）- 6 个月后
1. **Month 1**：Ready/Advance 重构
2. **Month 2**：动态成员变更 + RocksDB 集成
3. **Month 3**：MVCC Compaction + 生产验证

---

## 成功标准

### v1.1 发布标准
- ✅ 所有集成测试通过
- ✅ 性能达到 2,000 writes/s（单节点）
- ✅ Prometheus 指标可用
- ✅ 日志结构化且可配置
- ✅ 错误处理覆盖主要场景
- ✅ 运维文档完整

### v1.2 发布标准
- ✅ TLS 加密通信可用
- ✅ 基础认证机制可用
- ✅ ReadIndex 优化生效（读性能提升 2x）
- ✅ 慢 follower 可通过快照追赶
- ✅ 性能优化后达到 5,000 writes/s

### v2.0 发布标准
- ✅ 支持动态成员变更
- ✅ RocksDB 存储引擎可选
- ✅ MVCC compaction 可用
- ✅ 生产环境验证通过

---

## 风险与缓解

| 风险 | 影响 | 概率 | 缓解措施 |
|-----|-----|-----|---------|
| 集成测试不稳定 | 高 | 中 | 增加重试逻辑，隔离测试环境 |
| 性能未达标 | 高 | 中 | 提前性能分析，识别瓶颈 |
| TLS 配置复杂 | 中 | 高 | 提供自动证书生成脚本 |
| 动态成员变更复杂 | 高 | 高 | 分阶段实现，充分测试 |

---

## 总结

当前 MVP v1.0 已完成核心功能实现，下一步重点是：
1. **稳定性**：通过集成测试和错误处理提升
2. **可观测性**：通过日志和指标完善
3. **性能**：通过基准测试和优化提升

建议优先完成第二期任务（v1.1），为后续安全性和性能优化打下基础。
