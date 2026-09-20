# MiniKV 代码导读

第一次接触本项目可先读[完整学习指南](learning-guide.md)，再做[动手实验](learning-labs.md)。本页保留为快速阅读路线。

## 从一条命令出发

执行 `minikv_cli put name MiniKV` 后，请求经历：

```text
client_main.cpp
  PutRequest { key="name", value="MiniKV" }
  KVService_Stub::Put
          |
          | Protobuf 序列化 + bRPC 网络传输
          v
kv_service.cpp: KVServiceImpl::Put
  Store::Put
          v
store.cpp
  检查 key/value 长度
  RocksDB::Put -> WAL / 内存表
          |
          v
Response { code=OK }
  ClosureGuard 调用 done -> bRPC 发送响应
          |
          v
client_main.cpp 打印 OK
```

服务端是一个长期运行的进程；每次执行命令行客户端会创建一个新的客户端进程。底层 Socket 收发由 bRPC 完成。

## 第一站：接口协议

文件：`proto/minikv.proto`，约 30 行。

- `PutRequest` 同时携带键和值；`KeyRequest` 供 Get/Delete 使用。
- `string key` 表达文本键；`bytes value` 表达任意二进制数据，允许 `\0`。
- `Response.code` 是业务结果。`NOT_FOUND` 是一次成功通信带回的业务状态。
- `required` 是当前 proto2 接口的字段约束；字段存在不代表非空，长度仍需要服务层验证。
- `cc_generic_services` 让 protoc 生成服务基类和客户端 Stub，以便 bRPC 使用。

CMake 在构建目录生成 `minikv.pb.h/.cc`，不要手动修改生成文件。改变协议后重新构建即可。

思考：为什么不能仅凭响应中的 `value.empty()` 判断键不存在？因为合法值本来就可以为空。

## 第二站：存储与对象生命周期

文件：`src/store.h`、`src/store.cpp`。

`Open` 只在启动时调用一次。`unique_ptr<rocksdb::DB>` 保存数据库句柄，Store 销毁时自动关闭，使用 RAII 管理资源生命周期。

Put/Get/Delete 首先检查键的大小，然后调用 RocksDB。Put 另外检查值长度；Get 接收输出字符串指针，并通过 Status 区分成功、找不到、非法参数、I/O 错误。

RocksDB 允许多个线程对同一个 DB 对象并发调用常规读写操作，所以这里没有额外加一把全局 mutex。打开、销毁 DB 不能与正在执行的请求并发发生。`write_options_` 在启动后不再修改，只被并发读取。

普通共享 `int` 的并发自增需要保护，而 RocksDB 的公共读写 API 已提供相应并发支持。但“Get 后计算再 Put”这样的多个操作组合，并不会自动成为一个原子事务。

默认 `WriteOptions.sync=true`、`disableWAL=false`。成功确认前会同步 WAL；数据仍可在之后刷入 SST 文件。WAL、内存表、SST、后台压缩属于 RocksDB，不是此项目自行实现的存储引擎。

## 第三站：RPC 方法

文件：`src/kv_service.cpp`。

每个方法做三件事：调用 Store、设置响应、让框架发送响应。`SetStatus` 将 RocksDB 错误映射到我们自己的协议错误码。

`brpc::ClosureGuard guard(done)` 在函数退出时调用完成回调。这里 `done` 不是一个新的线程，而是通知框架“请求处理结束”。忘记调用会导致请求无法正常完成；重复调用也不正确。局部 guard 能覆盖常规返回路径。

Get 仅在状态成功时设置 value，避免在存储错误时返回不明确的数据。

## 第四站：服务启动与关闭

文件：`src/server_main.cpp`。

按顺序创建 Store、service、Server。C++ 局部对象按相反顺序销毁，因此 Server 先退出，再销毁服务和数据库。`SERVER_DOESNT_OWN_SERVICE` 表示 service 由当前作用域管理，Server 不负责 delete 它。

`RunUntilAskedToQuit` 等待退出信号并停止、等待服务任务结束。我们显式开启 bRPC 的 SIGTERM 优雅退出开关，并在监听之前安装框架退出处理器。这是端到端测试发现并验证修复的一个真实集成问题。

`max_concurrency=64` 使用 bRPC 自带并发上限。达到上限时框架可以拒绝请求；它不是我们手写的线程池或令牌桶。`worker_threads=0` 保留框架默认设置；非零值是线程数量提示，不能承诺恰好对应那么多系统线程。

## 第五站：客户端

文件：`src/client_main.cpp`。

1. gflags 解析地址、超时、文件路径；剩余位置参数表示操作和数据。
2. Channel 表示客户端通信通道，初始化并不保证服务可达。
3. Stub 根据协议提供 Put/Get/Delete 方法。
4. Controller 记录本次调用的网络、超时等状态；每次调用应使用自己的 Controller。
5. 传入空的完成回调表示这里使用同步调用，返回后再检查结果。

先判断 `controller.Failed()`，再判断 `response.code()`。前者表示没能获得正常 RPC 结果，后者表示服务给出的业务结果。

`max_retry=0` 关闭客户端自动重试。即使 Put 相同的值通常可以重复执行，在并发覆盖场景下重试仍可能覆盖别人刚写入的新值。超时后也不能简单宣布“服务端没有写入”。

普通 get 输出后加换行；使用 `--output_file` 才能得到严格相同的原始字节。

## 第六站：如何知道它工作了

`tests/store_test.cpp` 使用真实临时 RocksDB，检查基本语义、1 MiB 边界、并发独立键访问和重新打开。没有使用会在 Release 构建中消失的 C assert。

`tests/integration_test.py` 启动真实服务端，使用真实 C++ 客户端测试，然后执行 SIGKILL、重启同一数据库、核对已确认的数据，最后检查 SIGTERM 正常退出。Python 在这里仅负责编排，不是服务实现。

它们证明当前功能在这些场景下正确，不证明系统已经具备生产容量或机器断电保障。并发独立键测试也不等于证明多操作事务成立。

## 下一阶段的切入点

现已提供 `src/bench_main.cpp` 和 `scripts/compare-wal.py`。前者使用起跑屏障协调多个原生线程，每个线程执行同步 RPC 并维护自己的结果，结束后统一合并；后者在临时数据库中编排 WAL 同步策略对比。参见[性能测量](benchmarking.md)。

TTL 扩展仍未实现：它需要设计过期时间编码、读时判定和后台清理，并保证清理过程不会删除刚被其他请求更新的数据。参见[路线图](implementation-plan.md)。
