# 验证记录

日期：2026-09-19。环境：Ubuntu 24.04 / WSL 2 / x86_64。

## 版本

- GCC/G++ 13.3.0；CMake 3.28.3；Python 3.12.3。
- bRPC 1.18.0，Git SHA `94f1bbd32845a45f0218dfe43e25791252bdcb72`。
- Protobuf 3.21.12；RocksDB Ubuntu 包 8.9.1-2；gflags 2.2.2。
- bRPC 使用 Release 共享库；MiniKV 使用 Debug 构建。

## 实测结果

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

## 结论边界

- SIGKILL 是进程级故障，不是 WSL VM 崩溃、系统断电或磁盘故障。
- 测试使用临时数据库，不碰正常启动脚本的数据目录。
- 未做性能基准，因此没有 QPS 或 P99 结论。
- 未验证高并发极限、全部网络故障模式或生产部署；当前版本是单机功能基线。
