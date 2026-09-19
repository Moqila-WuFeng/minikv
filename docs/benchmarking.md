# RPC 性能测量

`minikv_bench` 测量完整 RPC 路径，包括客户端、序列化、网络、服务端和 RocksDB，不是存储引擎的独立基准。工具不修改服务协议，也不新增运行时依赖。

## 隔离对比实验

先运行 `bash scripts/bootstrap.sh` 安装依赖，再在仓库根目录创建 Release 构建。Debug 构建适合排错，不应与 Release 数据混合比较。

```bash
export MINIKV_HOME="${MINIKV_HOME:-$HOME/projects/minikv}"
cmake -S . -B "$MINIKV_HOME/build-release" \
  -DCMAKE_BUILD_TYPE=Release -DBRPC_ROOT="$MINIKV_HOME/.deps/brpc-install"
cmake --build "$MINIKV_HOME/build-release" -j2
ctest --test-dir "$MINIKV_HOME/build-release" --output-on-failure
python3 scripts/compare-wal.py \
  --build-dir "$MINIKV_HOME/build-release" \
  --output benchmark-results/wal-comparison.json \
  --requests 10000 --keys 1000 --warmup 200 \
  --concurrency 1 4 --operations put get mixed --repeats 3
```

Python 脚本需要 Python 3.11+，已在 Ubuntu 24.04 的 Python 3.12 上验证。它为每个 case 启动独立服务端，使用操作系统临时目录下的新数据库、独立键前缀与随机回环端口。只有自己的子进程报告启动成功且端口可连接后才发送请求。不会打开普通启动脚本的 `data/`。进程退出后删除本轮临时数据库；发生异常或超时也会回收子进程。

每组分别测试 `sync_writes=true/false`，均保留 WAL。重复轮次之间交替同步策略的执行顺序，减少固定顺序的影响；其他顺序保持不变。每轮默认最多 120 秒（包含预填充和预热），可用 `--case-timeout` 调整。任一 case 失败则整体退出非零，不生成伪装成完整实验的报告。

输出路径必须尚不存在。报告包含所有原始结果、每组吞吐中位数及范围、各轮 P99 的中位数、实际 CMake 构建模式、CPU 型号、内核、依赖版本、源代码提交状态和可执行文件 SHA-256。报告中的 P99 中位数不是把所有请求合并后的 P99。环境信息不包含主机名和用户名；原始报告默认被 Git 忽略。

源码位置来自构建目录的 `CMAKE_HOME_DIRECTORY`，不是运行器自身的位置。`source_checkout_*` 字段描述运行时该 checkout 的状态，不证明二进制一定由该提交生成；需要先重新构建，结合二进制 SHA-256 记录比较。Git 信息无法读取时显式标记为不可用。

## 单独运行客户端

仅针对专用测试实例运行。例如默认 Debug 构建的入口：

```bash
bash scripts/run-bench.sh --server=127.0.0.1:18081 \
  --operation=mixed --requests=10000 --concurrency=4 \
  --keys=1000 --value_size=128 --read_percent=50 --seed=1
```

**默认预填充会写入并覆盖 `minikv-bench:0` 等键，即使 `--operation=get` 也是如此。** 客户端不会自动删除这些键。不要对业务数据库执行；更换 `--key_prefix` 只能改变命名空间，不能替代隔离数据库。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `operation` | `mixed` | `put` 随机覆盖、`get` 随机读取、`mixed` 混合 |
| `requests` | 10000 | 所有线程合计的测量请求数，上限 1000000 |
| `concurrency` | 4 | 原生客户端线程数，上限 256，不超过请求数 |
| `keys` | 1000 | 工作集键数，上限 1000000 |
| `value_size` | 128 | 二进制值字节数，0 到 1048576 |
| `warmup` | 100 | 测量前串行预热请求数，可为 0 |
| `read_percent` | 50 | mixed 的读概率，不保证每轮恰好达到该比例 |
| `seed` | 1 | 值内容及各线程随机序列种子 |
| `prefill` | true | 测量前写入全部键；关闭后由调用者准备匹配的数据 |
| `timeout_ms` | 2000 | 单次 RPC 和连接超时，禁用自动重试 |

数值参数校验失败返回 2；任何测量错误、预填充或预热失败、输出失败返回 1；全部成功返回 0。预填充或预热失败时不生成测量报告。测量期间的失败仍计数并输出 JSON，不会将 `NOT_FOUND` 当作成功读取。

## 测量口径

1. 先预填充，再串行预热，两者不进入测量请求数或计时。
2. 所有工作线程到达起跑屏障后统一释放。总耗时从释放屏障到最后线程完成，包含调度与负载生成开销，不包含事后排序和 JSON 输出。
3. 每个线程最多一个未完成 RPC，是闭环负载。Channel 和 Stub 共享；请求、响应、Controller、计数器和延迟样本各自独立。
4. 每个请求的延迟使用单调时钟，覆盖请求构造、同步 RPC 与读取内容验证。成功及失败请求都进入 `attempt_latency_us`。
5. `success_ops_per_second = succeeded / elapsed_seconds`；另有尝试吞吐。错误分为 `rpc_errors`、`application_errors`、`validation_errors`，四类结果之和等于 `attempted`。
6. P50/P95/P99 使用 nearest-rank：排序后取第 `ceil(p * N)` 个样本。保留全部样本，单次至多一百万个；没有直方图量化误差，但短测量的尾部统计仍可能不稳定。

每个键使用同一个种子生成的二进制值；Get 会核对完整内容。Put 测量的是已有键覆盖，不是持续新增键。固定种子便于重放单线程负载，但多线程到达顺序不确定，且不同 C++ 标准库的随机分布实现可能不同。

## 解释结果时的限制

- 小工作集和预填充会使读缓存较热；相同值重复写入也可能受压缩影响。这不是冷读、海量数据或不可压缩的唯一值负载。
- 客户端和服务端在同一台机器时共享 CPU；WSL、宿主机负载和文件系统会影响结果。
- 闭环请求会在服务变慢时自动降低发送速率，存在 coordinated omission 的解释限制；P99 不是固定外部到达率下的排队延迟。
- 线程数、RPC 并发上限、键空间和数据大小都会影响结果；只比较配置一致的 case。
- `sync_writes=false` 可能更快，但不提供与同步写相同的系统故障持久性。不能将两者当作语义相同的优化。
- 本轮不包含 Delete、持续时间驱动的负载、开环限速、远程机器对比或长期后台压缩稳定性测试。

## 设计参考与归属

- [Apache bRPC 1.18.0 官方 C++ 客户端](https://github.com/apache/brpc/blob/94f1bbd32845a45f0218dfe43e25791252bdcb72/example/echo_c%2B%2B/client.cpp)：Channel/Stub 共享、每次调用独立 Controller 的用法。
- [RocksDB db_bench 说明](https://github.com/facebook/rocksdb/wiki/Benchmarking-tools)：区分预填充、读取、覆盖负载以及显式记录参数的组织方式。

以上是 API 用法及设计思路参考，没有复制上游压测工具源码。工作负载调度、计数、统计、隔离运行器及测试在本仓库独立实现，采用 Apache-2.0。原有第三方许可证仍见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
