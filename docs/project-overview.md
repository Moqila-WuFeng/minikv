# 项目全貌与验收边界

## 项目定位

MiniKV 0.2 是一个功能闭环的单机持久化键值 RPC 服务，不是 Redis 兼容产品，也不是分布式数据库。
提供数据读写、过期管理、故障恢复验证、可重复的性能测量和可读的实现文档。
“完成”指以下范围可构建、可运行、可验证，不表示生产环境的全部要求已经满足。

## 一条完整请求

```text
CLI -> Protobuf request -> bRPC Channel / Stub
    -> KVServiceImpl -> validation -> striped key lock
    -> RocksDB WriteBatch (value + TTL metadata) -> WAL
    -> application status -> RPC response -> CLI exit code
```

Get 在锁内检查 TTL，再读取值；后台扫描只负责回收逻辑上已过期的数据。
RPC 超时不证明写入失败，客户端禁用自动重试，避免隐式覆盖和 TTL 延长。

## 能力与证据

| 能力 | 项目实现 | 验证入口 |
| --- | --- | --- |
| 服务接口 | Protobuf 契约、参数边界、错误映射、CLI | `rpc_integration` |
| 持久化接入 | DB/列族生命周期、WAL 策略、原子元数据更新 | `store_contract`、恢复用例 |
| TTL | 持久化截止时间、读时判定、分段锁、游标清理 | `ttl_contract` |
| 并发 | RPC 并发限制配置、同键协调、多线程压测 | 存储与压测契约测试 |
| 可恢复性 | 关闭顺序、SIGKILL 后重开与过期判定 | `rpc_integration` |
| 测量 | 确定性负载、分阶段计数、分位数、隔离对比 | 四组 benchmark 测试 |
| 可复现构建 | 固定 bRPC 提交、Debug/Release、GitHub Actions | `scripts/bootstrap.sh`、CI |
| 开源交付 | Apache-2.0、自有/第三方边界、参考来源 | `THIRD_PARTY_NOTICES.md` |

## 必须讲清的取舍

- 复用 bRPC 网络/RPC 调度和 RocksDB WAL/LSM，不宣称自研网络框架或存储引擎。
- 用额外列族保留旧版原始二进制数据，代价是增加一次元数据查询和相关写放大。
- 分段锁比单一全局锁允许更多并行，但锁段碰撞、同步写会降低吞吐；未宣称性能提升。
- 读时过期保证接口语义；后台分批回收降低单轮工作量，但不保证立即释放磁盘空间。
- 默认同步 WAL 强调确认写的持久性；关闭同步是在改变故障语义，不是无代价优化。
- 基准是闭环请求模型，同机、小工作集的结果不能推算线上容量。

## 推荐阅读与演示

先看 [学习指南](learning-guide.md)，再按 [动手实验](learning-labs.md) 操作，
最后看 [TTL 设计](ttl-design.md) 和 [性能测量](benchmarking.md)。
演示顺序：构建和测试、CRUD/二进制文件、TTL 到期和覆盖、重启恢复、混合压测 JSON。
故障演示优先运行隔离集成测试，不对正常数据目录做 kill、清理或实验。

## 不在当前范围

复制、分片、Raft、跨键事务、CAS、Redis 协议、认证、TLS、线上备份恢复平台、
独立存储执行队列与生产级监控告警均未实现。
默认只监听回环地址，不应暴露到不可信网络；没有生产容量、安全认证或断电耐久性结论。
这些是明确的产品边界，而不是为了宣称完整而应全部补齐的功能。
