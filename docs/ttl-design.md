# TTL 设计与验证

## 接口契约

`PutRequest.ttl_ms` 是新增的可选 `int64` 字段，编号为 3；原字段编号不变。
省略或传 0 表示永久保存；正数表示从服务端执行 Put 时开始计时的毫秒数。
负数或截止时间溢出返回 `INVALID_ARGUMENT`，原值不变。
截止时间满足 `now >= expires_at` 时，Get 返回 `NOT_FOUND`。
写入和网络传输会消耗 TTL，因此极短 TTL 可能在客户端收到成功响应前已经到期。

```bash
bash scripts/run-client.sh --ttl_ms=2000 put session example
bash scripts/run-client.sh get session
sleep 3
bash scripts/run-client.sh get session
# NOT_FOUND，退出码 2
```

普通 Put 会同时清除原来的 TTL；带 TTL 的 Put 替换值并重新设置截止时间。
Delete 同时删除值和过期元数据；删除不存在或已经过期的键仍成功。
旧客户端可以连接新服务端，缺少 TTL 字段等同于 0。
**不能用新客户端向旧服务端请求 TTL**：旧版 Protobuf 服务会忽略未知字段，不能保证过期。

## 数据格式与兼容

使用两个 RocksDB column family（列族）：

| 列族 | key | value |
| --- | --- | --- |
| `default` | 用户键 | 原始二进制值，不加封装头 |
| `minikv_expiry_v1` | 用户键 | 正整数十进制 Unix 毫秒截止时间 |

没有过期元数据的值是永久值。旧版数据库的默认列族保持原样，首次打开时创建元数据列族，
不遍历、不重新编码既有值，因此任何二进制前缀都不会被误认为内部标记。
Put/Delete 使用同一 WriteBatch 原子更新两个列族，避免“值已经写了但 TTL 没写”的中间状态。
WAL 同步开关仍由 `--sync_writes` 控制，原子性不等于断电持久性。

升级前停止服务并备份整个数据库目录；不得复制正在写入的数据库当作可靠备份。
升级后 **不支持直接降级至 0.1.x**：旧服务只打开默认列族，不能处理新增列族和过期语义。
需要回退时停止新服务，恢复升级前完整备份。不要手工删列族或 RocksDB 的 LOCK 文件。

## 并发安全

原来的单次 RocksDB Get/Put 已由引擎保证线程安全；TTL 引入了跨调用业务约束：

1. Get 读取截止时间，再读取值，不能混用两个不同版本。
2. 清理器判断过期后删除，不能删除期间被覆盖的新值。

Store 使用 64 个 `std::mutex`，按键哈希选择同一把锁。Put/Get/Delete 和清理单个键都遵守该锁。
同一键的上述操作串行；不同锁段可以并行，哈希碰撞只影响并发度，不影响正确性。
锁覆盖同步磁盘 I/O，因此不能称为无锁或高并发极限优化；这是当前小型服务的清晰一致性方案。
客户端的多个 RPC 仍不是事务，例如 Get 后 Put 不能实现可靠的并发递增。

## 后台回收

Get 只检查逻辑过期，不负责磁盘删除，因此读取正确性不依赖后台清理进度。
`ExpiryWorker` 每隔 1000 ms 调用一次 `SweepExpired`，每轮最多扫描 256 个元数据条目。
两个参数为 `--ttl_sweep_interval_ms` 和 `--ttl_sweep_batch_size`，后者范围 1..10000。

扫描按用户键排序，保存游标，下次从游标之后继续，到末尾后回绕。
每轮创建并释放迭代器，不长期持有 RocksDB 快照；不用永久值填充扫描工作集。
迭代器可能看到旧版本，因此每个候选键都必须先取得分段锁，再重新读取当前截止时间。
不能根据旧迭代器上的时间直接 Delete，也不能“先检查、释放锁、再删除”。
扫描条目数有界，但 I/O 时长并无硬上限；清理积压不影响 Get 的逻辑过期。
持续插入和大数据量下不保证固定的回收时延。Delete 写入墓碑后，磁盘空间何时回收由 RocksDB compaction 决定。
遇到存储错误会记录日志；游标前移使后续键仍有清理机会，回绕后再次处理失败条目。
损坏元数据作为存储错误返回，不把它当永久值放行，也不自动删除可能有用的数据。

## 时钟与关闭

持久化使用系统墙上时钟，而非进程内 steady clock，才能跨重启判断时间。
因此系统时间前跳可能提前过期，回拨可能延迟过期；已经删除的数据不会因回拨恢复，
尚未清理的数据在回拨后可能再次落入有效期。需要准确时钟同步，不能承诺单调时钟语义。
测试通过注入假时钟验证边界，不依赖毫秒级 sleep。

对象创建顺序为 Store、ExpiryWorker、Service、Server；销毁顺序相反。
先停止 RPC，再唤醒并 join 清理线程，最后销毁列族句柄和 DB。
条件变量让退出不必等满扫描间隔，但正在执行的存储操作仍需完成。

## 测试与依据

`ttl_contract` 使用真实 RocksDB 和可控时间，覆盖旧数据升级、截止时间边界、TTL 清除/重置、
非法时长不修改旧值、删除元数据、分页扫描、重开数据库和清理并发。
`rpc_integration` 验证命令行 TTL、错误退出码及 SIGKILL 后的过期语义。

参考 RocksDB 官方 [Column Families](https://github.com/facebook/rocksdb/wiki/Column-Families)
的跨列族原子 WriteBatch 与句柄生命周期约定。
没有直接使用 [DBWithTTL](https://github.com/facebook/rocksdb/wiki/Time-to-Live)：
该方案以 compaction 删除为核心，不能直接满足本项目在 Get 上严格判断截止时间的接口要求。
本项目实现自己的接口语义和协调逻辑，没有复制第三方 TTL 实现，也没有增加运行时依赖。
