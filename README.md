# MiniKV

基于 **C++17、Apache bRPC、Protocol Buffers 和 RocksDB** 的单机持久化键值 RPC 服务。

提供明确的读写语义、命令行客户端和自动化恢复测试。网络传输与存储引擎使用成熟开源组件，项目实现接口契约、输入校验、错误映射、资源管理及端到端验证。

## 功能

- `Put / Get / Delete`，支持覆盖写入、空值和幂等删除。
- RocksDB 持久化，默认开启 WAL 同步写。
- 非空文本键，最多 1024 字节；二进制值，最多 1 MiB。
- C++ CLI 支持文本参数及二进制文件导入、导出。
- 独立的业务错误码与 RPC 错误处理，客户端不自动重试。
- bRPC 内置并发控制，默认最大并发请求数为 64。
- SIGINT / SIGTERM 优雅退出；SIGKILL 后已确认数据恢复测试。

## 架构

```text
minikv_cli -> Protobuf / bRPC -> KVServiceImpl -> Store -> RocksDB
                                  |                       |
                            application status       WAL / SST
```

| 路径 | 职责 |
| --- | --- |
| `proto/minikv.proto` | RPC 方法、消息和业务错误码 |
| `src/store.*` | RocksDB 生命周期、输入边界和键值操作 |
| `src/kv_service.*` | RPC 方法实现与存储错误映射 |
| `src/server_main.cpp` | 参数、启动与优雅关闭 |
| `src/client_main.cpp` | 命令行解析和同步 RPC 调用 |
| `tests/` | 存储契约与真实进程集成测试 |
| `scripts/` | 依赖构建、测试和启动入口 |

## 构建

已验证环境：Ubuntu 24.04 / WSL 2 / x86_64、GCC 13.3、CMake 3.28。需要网络访问，以及安装缺失系统包时的 `sudo` 权限。

克隆仓库并构建：

```bash
git clone https://github.com/Moqila-WuFeng/minikv.git
cd minikv
bash scripts/bootstrap.sh
```

脚本检查系统依赖，拉取并校验 bRPC **1.18.0** 的固定提交 `94f1bbd32845a45f0218dfe43e25791252bdcb72`，构建共享库、MiniKV 并运行测试。其他库来自 Ubuntu 系统包，实测版本见[验证记录](docs/verification.md)。

依赖、构建产物和数据库默认位于 `~/projects/minikv` 下的 `.deps/`、`build-service/` 和 `data/`。源码可放在任意目录。通过 `MINIKV_HOME` 改变产物位置，`MINIKV_JOBS` 设置编译并行度，默认 2；启动和测试时应保持相同的 `MINIKV_HOME`。

```bash
export MINIKV_HOME="$HOME/projects/minikv"
cmake --build "$MINIKV_HOME/build-service" -j2
ctest --test-dir "$MINIKV_HOME/build-service" --output-on-failure
```

## 使用

在仓库根目录启动服务，默认监听 `127.0.0.1:18081`：

```bash
bash scripts/run-server.sh
```

在另一终端执行：

```bash
bash scripts/run-client.sh put name MiniKV
bash scripts/run-client.sh get name
bash scripts/run-client.sh delete name
bash scripts/run-client.sh get name
```

结果依次为 `OK`、`MiniKV`、`OK`、`NOT_FOUND`。最后一条的退出码为 2。

二进制文件：

```bash
bash scripts/run-client.sh --value_file=/path/to/input.bin put blob
bash scripts/run-client.sh --output_file=/path/to/output.bin get blob
```

普通 `get` 在值后附加换行；需要原始字节时使用 `--output_file`，该选项会覆盖指定文件。文件路径选项放在命令之前；以 `-` 开头的位置参数前使用 `--` 结束选项解析。运行 `--help` 查看参数。

客户端退出码：0 成功；1 RPC 或标准输出错误；2 键不存在；3 参数或文件错误；4 服务端存储错误。

## 数据语义

| 情况 | 行为 |
| --- | --- |
| 重复 Put | 覆盖旧值；并发写入顺序由实际存储操作顺序决定 |
| 删除不存在的键 | 返回成功 |
| 空值 | 合法，与键不存在区分 |
| RPC 超时 | 写入结果可能未知，不自动重试 |
| 同一数据库被另一进程打开 | 启动失败 |
| 默认同步写 | WAL 同步完成后才确认成功 |
| `--sync_writes=false` | WAL 仍开启，但系统故障可能丢失最近确认的写入 |

持久性依赖操作系统、文件系统与硬件正确履行同步语义。SIGKILL 恢复测试不等价于断电测试。多个 RPC 操作的组合不构成事务。

## 测试

CTest 包含两组测试：

- `store_contract`：CRUD、覆盖、空值、二进制、长度边界、数据库锁、四线程独立键读写和数据库重开。
- `rpc_integration`：真实 C++ 客户端与服务端、退出码、标准输出失败、1 MiB 二进制文件、并发请求、SIGKILL 恢复、删除持久化、SIGTERM 退出及服务不可达。

测试使用临时数据库并回收子进程，不操作正常服务的数据目录。详细结果见[验证记录](docs/verification.md)。

## 当前边界

MiniKV 当前是单机服务，尚不支持 TTL、复制、分片、事务、认证、TLS 或独立存储执行队列。默认仅监听回环地址，不应直接暴露到不可信网络。

RocksDB 同步 I/O 在 RPC 回调中执行，可能阻塞工作线程。键值大小限制在消息解码后检查，不代表完整的网络层内存保护。尚未进行性能基准测试，不提供 QPS、P99 或生产容量承诺。

## 文档

- [代码导读](docs/code-tour.md)
- [架构与路线图](docs/implementation-plan.md)
- [验证记录](docs/verification.md)
- [第三方组件与许可证](THIRD_PARTY_NOTICES.md)

## 许可证

MiniKV 自有代码和文档采用 [Apache License 2.0](LICENSE)，参见 [NOTICE](NOTICE)。第三方组件保留各自版权及许可证，不因本项目的许可证而改变。

本仓库发布源代码与许可证记录，不包含第三方库的源码副本、编译产物、数据库或凭据。依赖来源、许可选项及完整通知记录见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
