# 验证记录

日期：2026-09-19。环境：Ubuntu 24.04 / WSL 2 / x86_64。

## 版本

- GCC/G++ 13.3.0；CMake 3.28.3；Python 3.12.3。
- bRPC 1.18.0，Git SHA `94f1bbd32845a45f0218dfe43e25791252bdcb72`。
- Protobuf 3.21.12；RocksDB Ubuntu 包 8.9.1-2；gflags 2.2.2。
- bRPC 使用 Release 共享库；MiniKV 使用 Debug 构建。

## 初始 RPC 版本结果

命令：

```bash
ctest --test-dir "${MINIKV_HOME:-$HOME/projects/minikv}/build-service" --output-on-failure
```

当前两组测试均通过：

| 测试 | 覆盖 |
| --- | --- |
| store_contract | Put/Get/Delete、覆盖、空值、二进制、长度边界、数据库独占锁、四线程独立键读写、关闭后重开 |
| rpc_integration | 真实客户端/服务端、错误退出码、1 MiB 二进制文件、四路并发调用、SIGKILL 后已确认数据恢复、删除持久化、SIGTERM 优雅退出、服务不可达 |

交付入口也已单独运行验证：`bash scripts/bootstrap.sh` 完整执行成功，内部两组 CTest 均通过；`run-server.sh` 与 `run-client.sh` 在随机回环端口和临时数据库下完成 `put lesson ready` / `get lesson`，得到 `OK` / `ready`，服务端正常退出。全部 Shell 脚本通过 `bash -n`。

已观察到测试先于实现失败。第一轮完整集成测试发现 SIGTERM 默认直接退出的问题；显式启用 bRPC `graceful_quit_on_sigterm` 并提前安装处理器后，通过相同测试。

MiniKV 自有代码构建开启 `-Wall -Wextra -Wpedantic`，本次构建未出现警告。bRPC 上游构建存在 deprecated API、unused variable 等警告，构建成功；不能声称全部第三方代码零警告。

独立代码审查发现客户端标准输出延迟刷新可能掩盖写入失败。新增 `/dev/full` 重定向回归测试，确认旧实现错误返回成功；显式刷新并检查输出流后，该用例与原有测试全部通过。

## 基准工具验证

新增 `minikv_bench`、统计单元测试及隔离实验运行器。Debug 和 Release
构建均通过以下六组 CTest：原有 `store_contract`、`rpc_integration`，以及
`bench_stats`、`benchmark_contract`、`benchmark_runner`、`benchmark_runner_unit`。

新增覆盖：精确分位数、请求数不能被线程数整除、请求数小于线程数、
空值和 1 MiB 值、预填充与预热隔离、混合负载 0%/100% 读比例、参数上限、
NOT_FOUND、读回内容不匹配、RPC 不可达、标准输出失败、报告不覆盖及实验超时。
新功能测试在实现前观察到缺失功能失败，完成后通过。

基准工具的源代码审查未发现需要修复的 C++ 问题；自动化测试仍是主要验收依据。
运行器审查发现端口归属和源码元数据问题，已通过先失败后通过的回归测试修复。
测试驱动的外层超时现在清理独立进程组，参数错误检查不再依赖已有报告文件造成的失败。
最终运行器完成 36 个 Release 实验 case，共 72000 次测量请求全部成功。
原始结果及适用范围见 [WAL 对比样本](benchmarks/2026-09-19.md)。
Shell 启动脚本通过 `bash -n`。上游依赖及 API 来源见
[性能测量说明](benchmarking.md)和[第三方声明](../THIRD_PARTY_NOTICES.md)。

## 0.2 TTL 验收（2026-09-21）

WSL / Ubuntu 24.04 中分别重新构建 Debug 和 Release，两种构建的七组 CTest 均通过：
`store_contract`、`ttl_contract`、`rpc_integration`、`bench_stats`、`benchmark_contract`、
`benchmark_runner`、`benchmark_runner_unit`。

新增验证范围：

- 默认列族原始二进制旧数据升级，TTL 列族重开和持久化截止时间。
- 假时钟精确到期、系统时钟回拨的已声明语义、负时长、截止时间溢出、覆盖清除 TTL。
- 每轮扫描数量限制、游标推进、值和元数据均删除，以及损坏元数据后的清理进度。
- 受控交错：清理器已创建包含过期候选的迭代器，在处理首键时暂停，另一个线程把后续候选改为永久值，恢复清理后新值仍在。
- 后台线程实际执行清理、清理与关闭并发、长扫描间隔不拖延关闭、端口占用导致启动失败时线程回收。
- 真实 CLI 的 TTL 使用和参数错误，SIGKILL 后在原截止时间判断到期。

TTL 首次 RPC 测试在未实现 `--ttl_ms` 时失败；损坏元数据推进测试在修复前失败，修复后通过。
独立审查指出并发与后台清理证据不足，随后增加上述受控测试，而不是仅凭 Get 的 NOT_FOUND 推断物理删除。
这里的删除指两列族都返回 NotFound，不代表 SST 空间已经被 compaction 回收。
关闭测试验证正常回收及有界返回，但未穷举线程调度，也未精确控制析构函数进入时点。

新增 GitHub Actions 配置在 Ubuntu 24.04 构建依赖并运行 Debug/Release 测试。
本地验证结果与远程 CI 状态分开：远程运行记录以仓库 Actions 页面为准。
2026-09-19 性能报告保留为旧版本历史样本，不作为新增 TTL 元数据与锁之后的性能结论。

## 结论边界

- SIGKILL 是进程级故障，不是 WSL VM 崩溃、系统断电或磁盘故障。
- 测试使用临时数据库，不碰正常启动脚本的数据目录。
- 初始 RPC 版本未包含性能基准；后续新增的工具不等同于生产容量认证。
- 未验证高并发极限、全部网络故障模式或生产部署；当前版本是单机功能基线。
