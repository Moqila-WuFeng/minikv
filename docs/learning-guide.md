# MiniKV 学习指南

适合已经熟悉 C++ 基础语法、能够使用 Linux，但尚不熟悉多线程网络服务的读者。
本文对应包含 RPC 基准工具的版本；服务协议仍是 Put/Get/Delete，没有 TTL。

这不是要求从头实现一遍。先读懂一条请求为什么成功，再理解它为什么可能失败，最后研究并发和性能。

## 阅读路线

| 次序 | 本文章节 | 同时打开的文件 | 读完应能解释 |
| --- | --- | --- | --- |
| 1 | 程序如何组成 | [CMakeLists.txt](../CMakeLists.txt) | 编译、链接、运行有什么区别 |
| 2 | 一条 Put 的旅程 | [协议](../proto/minikv.proto)、[客户端](../src/client_main.cpp) | 请求怎么从客户端到达数据库 |
| 3 | 服务端与生命周期 | [服务](../src/kv_service.cpp)、[启动](../src/server_main.cpp) | 谁拥有对象，何时释放 |
| 4 | 存储与并发 | [Store](../src/store.cpp) | WAL、同步写和线程安全的边界 |
| 5 | 多线程压测 | [压测主程序](../src/bench_main.cpp) | 为什么这里不用全局计数锁 |
| 6 | 测试与实验 | [动手实验](learning-labs.md) | 如何用现象验证自己的理解 |

第一次只读到“一条 Put 的旅程”，然后做实验 1、2。第二次读生命周期和存储，第三次再读压测。
已有的[简短代码导读](code-tour.md)适合之后快速回顾；本文负责展开原因。

## 1. 先建立整体模型

### 1.1 这个项目是什么

MiniKV 是一个通过 RPC 访问的单机键值服务。可以把键 `name` 对应的值设置为 `MiniKV`，之后读取或删除。

```text
客户端进程                         服务端进程
minikv_cli                         minikv_server
参数 -> 请求对象 -> bRPC  ======>  bRPC -> KVServiceImpl -> Store -> RocksDB
输出 <- 响应对象 <- bRPC  <======  bRPC <- Response      <- Status / value
                                |
                                +-> 数据库目录：日志、元数据、SST 等
```

两边是不同进程，即使运行在同一台机器，也不直接共享普通 C++ 变量。请求跨进程需要网络通信。

RocksDB 是链接进服务端的嵌入式库，不是我们另外启动的一个数据库服务进程。

### 1.2 哪些是项目代码，哪些是依赖

| 能力 | 提供者 | MiniKV 做的事情 |
| --- | --- | --- |
| 网络收发、RPC 分发 | Apache bRPC | 配置服务、实现 RPC 方法、处理调用结果 |
| 消息编码与生成代码 | Protobuf | 定义 `.proto` 协议，使用生成的消息和 Stub |
| WAL、LSM、SST、压缩 | RocksDB | 管理 DB 句柄，设置写选项，封装键值操作 |
| 命令行选项 | gflags | 定义选项，校验组合，约定退出码 |
| 负载生成、统计、对比实验 | MiniKV | 多线程压测、JSON 报告、实验隔离和测试 |

集成库并不意味着没有需要解决的问题：输入边界、资源所有权、失败语义、关闭顺序和验证方法都由应用决定。
但不能把依赖的网络栈、线程调度器或存储引擎说成项目自行实现。许可和来源见[第三方声明](../THIRD_PARTY_NOTICES.md)。

### 1.3 当前没有什么

没有 TTL、复制、分片、分布式一致性、多操作事务、认证和 TLS。没有自研线程池、独立存储执行队列或自研 WAL。

默认只监听回环地址。即使改成其他地址可以连接，也不代表已经适合对外提供服务。

## 2. 从源文件到运行中的进程

### 2.1 四个阶段

```text
.proto --protoc--> minikv.pb.h / minikv.pb.cc
.cpp + 头文件 --编译--> 目标文件 .o
目标文件 + 库 --链接--> minikv_server / minikv_cli / minikv_bench
执行文件 --操作系统加载--> 进程及其线程
```

`#include` 让编译器看到类型和函数声明，不等于已经链接了函数实现。
因此“找不到头文件”和“undefined reference”通常是不同阶段的问题。

CMake 描述生成规则和依赖关系；它不是 C++ 编译器。`g++` 才执行 C++ 编译和链接的实际工作。

### 2.2 本项目的 CMake 目标

- `minikv_proto`：生成的协议代码；客户端和服务端都需要它。
- `minikv_store`：存储封装；CLI 不直接链接这个目标来操作数据库。
- `minikv_server`：服务入口和 RPC 实现，链接协议、存储和网络库。
- `minikv_cli`：一次命令执行一次 RPC，然后退出。
- `minikv_bench`：一个进程内创建多个线程，持续调用 RPC。

`target_include_directories` 配置头文件搜索路径；`target_link_libraries` 配置链接依赖。
`PUBLIC` 依赖会传播给使用该目标的其他目标，`PRIVATE` 则主要用于当前目标。

### 2.3 源码目录与构建目录

```bash
# 在仓库根目录执行
export MINIKV_HOME="${MINIKV_HOME:-$HOME/projects/minikv}"
cmake --build "$MINIKV_HOME/build-service" -j2
ctest --test-dir "$MINIKV_HOME/build-service" --output-on-failure
```

上述命令假设已运行过 `bash scripts/bootstrap.sh`。
生成的 `minikv.pb.h/.cc` 在构建目录，而不是 `src/`；不要手改生成代码。
修改 `.proto` 后重新构建，让生成规则自动运行。

构建成功只证明编译、链接成功，不证明请求语义正确。CTest 用实际行为弥补这部分验证。

## 3. 追踪一条 Put

以 `minikv_cli put name MiniKV` 为例。阅读时按函数名查找，不依赖可能随版本变化的行号。

### 3.1 解析命令行

在 [client_main.cpp](../src/client_main.cpp) 中，gflags 先消费 `--server`、`--timeout_ms` 等选项。
之后检查位置参数是否构成合法操作。例如没有文件输入时，Put 需要操作名、键和值。

参数不合法时可以在客户端直接失败，不必发送 RPC。但客户端校验不能替代服务端校验：其他客户端也可能访问服务。

### 3.2 创建请求对象

协议中的 `PutRequest` 有 `string key` 和 `bytes value`。客户端调用 `set_key`、`set_value` 填充它。

`bytes` 允许值含有零字节。`std::string` 自身也能保存零字节，只要使用正确的长度；并不是只能表示以 `\0` 结尾的文本。

协议里的 `required` 表示字段必须存在，不表示字符串必须非空。Store 仍检查键长为 1 到 1024 字节，值最多 1 MiB。
这里的文本键是接口约定；`Store::ValidateKey` 本身做的是字节长度检查，不是独立的 UTF-8 校验器。

### 3.3 Channel、Stub、Controller 分工

| 对象 | 含义 | 容易误解的地方 |
| --- | --- | --- |
| `brpc::Channel` | RPC 通信通道及选项 | 初始化成功不保证目标服务在线 |
| `KVService_Stub` | 生成的调用入口，例如 `Put` | 看起来像函数调用，实际可能跨进程、超时或断线 |
| `brpc::Controller` | 本次调用的状态、错误等 | 不应让多个并发请求复用同一个 Controller |
| `PutRequest` / `Response` | 本次请求与业务响应 | 网络失败时不能把响应当成有效业务结果 |

```cpp
stub.Put(&controller, &request, &response, nullptr);
```

最后一个参数为 `nullptr`，这里采用同步调用：当前调用线程等待完成，再执行下一行。
这不代表整个服务端只有一个线程，也不代表其他客户端必须一起等待。

### 3.4 框架把请求交给服务方法

客户端通过 bRPC 编码和发送请求；服务端的 bRPC 接收后调用
[`KVServiceImpl::Put`](../src/kv_service.cpp)。项目不手写 Socket 的收发循环或 TCP 分包解析。

服务方法的核心是：

```cpp
brpc::ClosureGuard guard(done);
SetStatus(store_.Put(request->key(), request->value()), response);
```

`store_` 是已有 Store 的引用，不是每来一个请求就新开一个数据库。
`SetStatus` 把 RocksDB 的状态映射成协议错误码，客户端不需要理解所有 RocksDB 内部错误类型。

### 3.5 Store 做校验，再调用引擎

[`Store::Put`](../src/store.cpp) 依次检查数据库已打开、键长度、值长度，再调用 `db_->Put`。
`write_options_` 决定是否同步 WAL；引擎操作返回后，状态沿调用链返回。

非法请求应在这里结束，不能先写入再报告参数不合法。

### 3.6 完成响应

`ClosureGuard` 在离开作用域时调用 `done`。`done` 是框架提供的完成通知，不是新建线程，也不是网络文件描述符。
它让框架继续完成响应处理；调用后不应再随意访问框架可能回收的请求、响应对象。

客户端首先检查通信结果，再检查业务结果。成功时打印 `OK`，最后主动刷新并检查标准输出。
这使“写数据成功但结果输出失败”不会被 CLI 错误地当成完全成功。

## 4. 三种失败不要混在一起

| 情况 | 发生在哪里 | CLI 的表现 |
| --- | --- | --- |
| `get` 一个不存在的键 | 服务端正常处理，但业务结果是 `NOT_FOUND` | 退出码 2 |
| 地址不可达或调用超时 | RPC 层无法获得正常结果 | `RPC_ERROR`，退出码 1 |
| 命令缺参数或文件打不开 | 客户端本地校验、输入输出 | 退出码 3 |
| RocksDB 返回未归入其他类别的存储错误 | 服务端存储层 | `STORAGE_ERROR`，退出码 4 |

空值不是“不存在”：`Put("k", "")` 后，Get 应返回成功和空内容，不能用 `value.empty()` 判断存在性。
本项目 Delete 不存在的键也返回成功，这属于接口约定。

### 超时不等于写入失败

```text
客户端发送 Put -> 服务端已经写入 -> 响应未及时到达 -> 客户端超时
```

此时客户端只知道没有及时获得结果，不能推出数据库没有变化。
本项目设置 `max_retry=0`，避免框架自动重试造成难以察觉的后果。

即使“把 k 设成 A”重复执行看似幂等，也要考虑中间另一个请求已经把 k 改成 B：旧请求重试会再次覆盖 B。
可靠重试通常还需要请求标识、去重、版本检查等设计；当前没有实现这些机制。

## 5. 对象生命周期比调用顺序更重要

### 5.1 谁拥有数据库

`Store` 使用 `std::unique_ptr<rocksdb::DB>` 持有数据库。成功打开后通过 `reset(database)` 接管所有权。
当 Store 销毁时，智能指针释放 DB 对象，不需要每条返回路径手写 `delete`。

RAII 的关键是“资源跟随对象生命周期”，不是“使用智能指针就自动线程安全”。

### 5.2 为什么服务端这样声明对象

```cpp
minikv::Store store;
// Open ...
minikv::KVServiceImpl service(store);
brpc::Server server;
```

局部对象按相反顺序销毁：先 Server，再 service，最后 Store。
service 中的引用要求 Store 活得更久；服务注册使用 `SERVER_DOESNT_OWN_SERVICE`，所以 bRPC 不负责删除这个栈对象。

正常关闭时，先停止接收和等待服务任务结束，再释放数据库。否则在途请求可能访问已释放的 DB，形成悬空引用。

### 5.3 SIGTERM 与 SIGKILL

SIGTERM 可以由程序处理，当前服务显式开启 bRPC 的优雅退出路径。
SIGKILL 不能被捕获，析构函数和退出清理都不能保证执行。它适合测试进程突然消失时的恢复，但不等于机器断电。

不要把“正常退出时会调用析构函数”推广成“任何退出都会调用析构函数”。

## 6. 持久化：内存、WAL 与 SST

从概念上区分三件事：WAL 记录用于恢复的更新，MemTable 接收内存中的数据，SST 保存刷到存储上的有序数据。
本项目没有自行实现这些结构，全部由 RocksDB 管理。

`sync_writes=true` 时，应用要求写操作返回成功前同步 WAL。它不意味着每个 Put 都生成一个 SST，也不意味着每次都执行完整压缩。
`sync_writes=false` 仍启用 WAL，只是不要求每次确认前完成相同的同步保证。

RocksDB 关于写选项和恢复的原始说明见[基本操作](https://github.com/facebook/rocksdb/wiki/Basic-Operations)与
[WAL 文档](https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-%28WAL%29)。持久性仍依赖文件系统和硬件正确兑现同步语义。

### 6.1 为什么 SIGKILL 后还能找回值

重新打开同一数据库时，RocksDB 利用已有持久化数据和日志恢复状态。
测试验证的是“特定场景中，确认成功的数据在服务进程被杀后仍可读取”，不是全部故障模式下的证明。

### 6.2 为什么不每个请求都打开数据库

打开和恢复数据库有开销，而且同一数据库目录受到引擎锁保护。
当前方案是在进程启动时打开一次，在整个服务期间复用 DB，所有请求结束后才销毁。

## 7. 并发：先找共享状态，再讨论锁

### 7.1 共享计数器为什么会出问题

多个线程同时对普通 `int` 执行 `counter++`，没有同步时构成 C++ 数据竞争，行为未定义。
“可能丢失更新”是帮助理解的直观例子，但语言层面的问题不只局限于少加几次。

可用互斥锁保护整个操作，或在只需原子计数时用 `std::atomic`。具体方案取决于要保护的操作，而不是看到多线程就加锁。

### 7.2 Store 为什么没有全局 mutex

RocksDB 的同一 DB 对象支持常规 Put/Get/Delete 并发调用，相关保证见[官方并发说明](https://github.com/facebook/rocksdb/wiki/Basic-Operations#concurrency)。
MiniKV 还满足两个使用条件：DB 在工作线程运行前打开，在工作结束后销毁；`write_options_` 初始化后只读。

这不表示 RocksDB 内部没有锁，也不表示 Store 的所有方法都能在任意时刻并发调用。
不要在处理请求的同时重新 Open、销毁 Store 或修改共享写选项。

### 7.3 线程安全不等于事务

设 k 原来是 0：

```text
请求 A：Get(k) -> 0
请求 B：Get(k) -> 0
请求 A：Put(k, 1)
请求 B：Put(k, 1)
最终：1，不是 2
```

单个 Get 和 Put 都可以是线程安全的，但组合的“读、计算、写”不是自动原子的。
只在客户端加本地 mutex 也不能约束其他客户端进程。当前协议没有原子递增、CAS 或事务。

### 7.4 三个并发数字

| 数字 | 所属位置 | 表示什么 |
| --- | --- | --- |
| `concurrency` | 压测客户端 | 生成负载的原生线程数量，每线程一次等待一个 RPC |
| `worker_threads` | 服务端 bRPC 配置 | 给框架的工作线程数量提示，不是整个进程线程总数 |
| `max_concurrency` | 服务端 bRPC 配置 | 框架内置的并发请求上限，不是连接数或 QPS |

这三个数不要求相等。超过并发上限可能被拒绝；这不等于实现了“每秒限制 N 次”的令牌桶算法。
RocksDB 的同步 I/O 当前直接发生在 RPC 回调中，可能占用工作线程等待；尚未使用独立存储队列。

## 8. 读懂多线程压测

### 8.1 先看阶段，不要先看 JSON 拼装

[`Run`](../src/bench_main.cpp) 的主要阶段为：

```text
校验参数 -> 创建通信对象和确定性值 -> 预填充 -> 预热
       -> 创建线程并等待就绪 -> 同时放行 -> 发请求并记录
       -> join -> 合并 -> 排序算分位数 -> 输出 JSON
```

预填充让读请求有数据可读；预热让最初的一些开销不混入正式测量。
预填充默认开启，因此 `--operation=get` 也会先写数据。只能对专用测试库运行。

### 8.2 请求数如何精确分配

每个线程先分得 `requests / workers` 次，前 `requests % workers` 个线程再多分一次。
例如 23 次请求、4 个线程，分配为 `6 + 6 + 6 + 5`，不会丢掉余数。

`workers = min(requests, concurrency)`，所以只有 1 次请求时不会创建 256 个空闲线程。

### 8.3 为什么结果计数不需要全局 atomic

`results` 在启动前完成分配。线程 i 只写 `results[i]`；它们不同时修改同一个计数器。
主线程在全部 `join()` 后才读取和汇总这些结果。

关键不是“vector 是线程安全的”，而是不同线程操作不同元素，且期间不改变 vector 的结构。
若在发请求时对共享 vector 执行 `push_back`，这个论证就失效了。

### 8.4 起跑屏障如何避免漏通知

`ready`、`start`、`cancel` 受同一 mutex 保护。
工作线程递增 ready，然后 `condition.wait(lock, predicate)` 等待 start；主线程等 ready 达到目标，再记录开始时间并设置 start。

`wait` 等待时会释放锁，被唤醒后重新取得锁并检查谓词。谓词可以应对虚假唤醒，也让“通知先发生”不等于丢失状态。
通知只是唤醒提示，真正决定能不能继续的是受锁保护的状态。

代码中的 `[&, i]` 按值捕获循环变量 i，其他所需对象按引用捕获。
这样每个线程保有自己的索引，而不是一起引用一个不断变化的循环变量。

### 8.5 为什么还要处理线程创建失败

创建到第几个线程时也可能抛出异常。如果直接退出，已经创建的线程可能一直等待起跑信号。
当前代码设置 cancel/start，唤醒并 join 已有线程后再向外报告异常。
线程执行期间的异常通过 `exception_ptr` 保存，join 后由主线程处理。

### 8.6 吞吐和延迟是两个维度

成功吞吐 = 成功请求数 / 整体测量时间。失败请求另记，但其延迟也保留在 `attempt_latency_us` 中。
一次读取只有在 RPC 正常、业务状态成功、读回数据匹配时才算成功。

对 1 到 100 微秒这 100 个样本，nearest-rank P50 为 50、P95 为 95、P99 为 99。
对只有 3 个样本的短测试，P99 会落到最大值；它不是稳定的尾延迟估计。

分位数不能随意平均。报告的 `median_of_run_p99_us` 是“每轮 P99 的中位数”，不是将所有轮次样本合并后的 P99。

### 8.7 闭环测量的边界

一个线程等到当前 RPC 完成，才发下一个。服务变慢时，请求发送速度也会跟着变慢。
因此本工具不能直接回答“外部每秒固定涌入大量请求时，队列会积压多久”。
完整口径、参数及限制见[性能测量文档](benchmarking.md)。

## 9. 测试告诉我们什么

| 测试组 | 验证的边界 | 不代表什么 |
| --- | --- | --- |
| `store_contract` | 直接调用 Store 的 CRUD、长度、并发、重开 | 不验证网络和全部同键交错 |
| `rpc_integration` | 真客户端、真服务、进程故障恢复 | 不等于断电、磁盘损坏测试 |
| `bench_stats` | 已知样本的统计计算 | 不等于测量方法适用于所有负载 |
| `benchmark_contract` | 工作负载、计数、读回校验和失败路径 | 不等于客户端无限扩展 |
| `benchmark_runner` | 实验编排、策略对比、报告保护及超时 | 不等于生产部署验证 |
| `benchmark_runner_unit` | 受控模拟启动竞态和元数据问题 | 不替代真实进程集成测试 |

存储测试用显式的 Check 抛出失败，不依赖可能被 `NDEBUG` 关闭的 C `assert`。
Python 用来编排进程和验证结果，并不是把 C++ 服务重新实现一遍。

## 10. 自测题

先用自己的话回答，再打开参考答案。

<details>
<summary>为什么客户端退出后，数据库里的值仍在？</summary>

客户端不是数据库所有者。数据写入服务端管理的 RocksDB；退出客户端不会删除服务端的数据库目录。

</details>

<details>
<summary>Channel 初始化成功，为什么第一次 Get 仍可能失败？</summary>

初始化通信配置不等于已经验证服务可达。实际请求仍可能遇到连接失败、超时等问题，要检查 Controller。

</details>

<details>
<summary>把 worker_threads 设成 4，为什么系统可能看到更多线程？</summary>

它是 bRPC 的工作线程提示，进程还可能存在框架辅助线程和 RocksDB 后台线程，不能把它理解为总线程上限。

</details>

<details>
<summary>sync_writes=false 是否意味着关闭 WAL？</summary>

不是。Store 显式保持 disableWAL=false；改变的是写入确认前的同步要求。

</details>

<details>
<summary>为什么先 join 再合并计数？</summary>

保证工作线程不再修改结果，且其写入对 join 成功后的读取可见。否则主线程可能与仍在写结果的线程发生数据竞争。

</details>

<details>
<summary>为什么 TTL 不能只增加一条定时 Delete？</summary>

过期信息需要持久化和明确的读语义；后台扫描看到旧值后，另一个请求可能写入新值。无条件 Delete 可能误删新数据。
这需要单独设计，不是当前版本已经解决的问题。

</details>

## 11. 读到什么程度算理解了

- 不看本文，能画出 CLI 到 RocksDB 再返回的调用路径。
- 能指出每个重要对象的所有者，解释正常关闭顺序。
- 能区分通信错误、业务错误和本地输出错误。
- 能解释共享计数器的数据竞争，以及当前压测为什么避免共享热路径计数。
- 能解释单次线程安全操作与多步原子事务的区别。
- 能给一组性能数字补上环境、参数、测量方法和限制。

接下来做[动手实验](learning-labs.md)。遇到不理解的地方，以“文件名 + 函数名 + 具体疑问”记录，比只说某个模块看不懂更容易定位。
