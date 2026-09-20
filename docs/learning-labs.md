# MiniKV 动手实验

配合[学习指南](learning-guide.md)阅读。每个实验都有目标、操作、预期结果和对应代码。
这些命令在 Ubuntu / WSL 的 Bash 中执行，不是在 Windows PowerShell 中执行。

不需要一次做完。建议先完成实验 1、2，再回到原理文档。

## 准备：目录与二进制

进入 MiniKV 仓库根目录，也就是包含 `CMakeLists.txt`、`src/`、`scripts/` 的目录。
源码位置与构建位置不是同一个概念，不要假定源码一定在 `~/projects/minikv`。

```bash
pwd
ls CMakeLists.txt src scripts
export MINIKV_HOME="${MINIKV_HOME:-$HOME/projects/minikv}"
export MINIKV_BIN="$MINIKV_HOME/build-service"
ls "$MINIKV_BIN/minikv_server" "$MINIKV_BIN/minikv_cli"
```

如果还没有构建产物，在仓库根目录运行 `bash scripts/bootstrap.sh`。
已有构建产物时只需 `cmake --build "$MINIKV_BIN" -j2`。

实验使用独立临时数据库，不使用常规启动脚本的 `data/`。下面示例使用端口 18091；
先运行 `ss -ltn 'sport = :18091'`，如果显示监听记录，请换一个空闲端口，并同步修改所有客户端地址。
不要为了腾出端口而随意杀掉现有进程。

## 实验 1：真正启动两个进程

**目标：** 理解服务端持续运行，CLI 是另外一个短生命周期进程。

终端 A 执行准备章节中的变量设置，然后运行：

```bash
export MINIKV_LAB="$(mktemp -d /tmp/minikv-study.XXXXXX)"
printf '实验目录：%s\n' "$MINIKV_LAB"
"$MINIKV_BIN/minikv_server" \
  --listen=127.0.0.1:18091 --db_path="$MINIKV_LAB/db"
```

应看到 `MiniKV listening on 127.0.0.1:18091`，且日志中的 DB 路径是刚创建的临时目录。
此时终端 A 不返回 Shell 提示符，因为服务端还在运行。若启动报错，先排错，不要继续向该端口发送请求。

终端 B 也要设置自己的变量。终端之间不会自动共享刚 export 的变量：

```bash
export MINIKV_HOME="${MINIKV_HOME:-$HOME/projects/minikv}"
export MINIKV_BIN="$MINIKV_HOME/build-service"
kv() { "$MINIKV_BIN/minikv_cli" --server=127.0.0.1:18091 "$@"; }
kv put name MiniKV
kv get name
```

预期先输出 `OK`，再输出 `MiniKV`。终端 B 每条命令结束后返回提示符，但终端 A 仍然运行。

可在终端 B 观察：

```bash
ss -ltnp 'sport = :18091'
ps -C minikv_server -o pid,nlwp,args
```

`pid` 是进程号，`nlwp` 是线程数量。系统上有多个 MiniKV 服务时会显示多行，按地址和 DB 参数辨认自己的实验进程。

**看代码：** [server_main.cpp](../src/server_main.cpp) 的 `RunUntilAskedToQuit`，以及 [client_main.cpp](../src/client_main.cpp) 的 `main`。

**自问：** 为什么关闭终端 B 的 CLI 不会把终端 A 的数据库一起销毁？

## 实验 2：空值、不存在、非法参数

在终端 B 执行，每次 `echo` 都紧接要观察的命令：

```bash
kv put empty ""
kv get empty
echo "$?"

kv get never-created
echo "$?"

kv get ""
echo "$?"

kv delete never-created
echo "$?"
```

| 操作 | 预期 | 退出码 |
| --- | --- | ---: |
| Get 空值 | 输出一个换行，业务成功 | 0 |
| Get 不存在的键 | stderr 包含 `NOT_FOUND` | 2 |
| Get 空键 | stderr 包含 `INVALID_ARGUMENT` | 3 |
| Delete 不存在的键 | 输出 `OK` | 0 |

`$?` 是上一条命令的退出码；如果中间插入了 `ls`，看到的就是 `ls` 的结果。

**看代码：** [协议错误码](../proto/minikv.proto)、[SetStatus](../src/kv_service.cpp)、[ValidateKey](../src/store.cpp)。

**自问：** 这三种 Get 都使用同一个 RPC，为什么不能仅凭字符串是否为空判断成功？

## 实验 3：二进制不等于可打印文本

以下操作只在新建的临时文件目录中生成输入和输出：

```bash
export MINIKV_BYTES="$(mktemp -d /tmp/minikv-bytes.XXXXXX)"
printf 'a\0b\377' > "$MINIKV_BYTES/input.bin"
kv --value_file="$MINIKV_BYTES/input.bin" put binary
kv --output_file="$MINIKV_BYTES/output.bin" get binary
cmp "$MINIKV_BYTES/input.bin" "$MINIKV_BYTES/output.bin"
echo "$?"
od -An -tx1 "$MINIKV_BYTES/output.bin"
```

预期 `cmp` 无输出，退出码为 0；`od` 显示 `61 00 62 ff`。
这说明值中间的零字节没有截断数据。普通 `get` 会附加换行，不适合做精确字节导出。

**看代码：** `std::ios::binary`、`std::string::size()`、`response.value().data()` 和 `ofstream::write`。

**自问：** 如果把长度错误地换成 `strlen(value.c_str())`，这个实验会发生什么？不要直接修改主分支验证这个错误。

## 实验 4：进程重启与数据库生命周期

终端 B：

```bash
kv put durable acknowledged
```

终端 A 按 Ctrl+C，等服务正常退出，然后在同一终端用同一个目录重启：

```bash
"$MINIKV_BIN/minikv_server" \
  --listen=127.0.0.1:18091 --db_path="$MINIKV_LAB/db"
```

终端 B 再运行 `kv get durable`，预期得到 `acknowledged`。

关键是复用原来的 DB 路径。若重新执行 `mktemp -d` 换了空目录，读不到旧键是正常现象，不是持久化失效。

突然退出恢复不要对其他服务使用 `kill -9`。在仓库根目录运行已有隔离测试即可：

```bash
ctest --test-dir "$MINIKV_BIN" -R '^rpc_integration$' --output-on-failure
```

它自己创建服务、临时数据库与随机端口，对自己启动的进程执行 SIGKILL，再重启检查数据，不会杀掉终端 A 的服务。

**看代码：** [集成测试](../tests/integration_test.py) 中 `server.kill()`、`start()`、`survives`。

**自问：** 进程被 SIGKILL 和整个操作系统掉电，丢失的数据范围为什么可能不同？

## 实验 5：业务失败与网络失败

先保持终端 A 正常运行，终端 B 执行 `kv get never-created`，结果应该是 `NOT_FOUND`。

然后终端 A 按 Ctrl+C，确认退出。终端 B 执行：

```bash
kv --timeout_ms=500 get durable
echo "$?"
```

预期 stderr 包含 `RPC_ERROR`，退出码为 1。具体网络错误文本可能因环境不同而变化。

**看代码：** 客户端先检查 `controller.Failed()`，再检查 `response.code()`。

**自问：** 如果把检查顺序颠倒，会不会把没有收到的响应误当成某种业务结果？

完成本实验后，终端 A 可按实验 4 的命令重启，继续下面的手动压测。

## 实验 6：观察线程与精确计数

确认目标端口仍是终端 A 的临时实验服务。终端 B 执行：

```bash
"$MINIKV_BIN/minikv_bench" --server=127.0.0.1:18091 \
  --operation=mixed --requests=23 --concurrency=4 \
  --keys=7 --warmup=5 --value_size=128 --key_prefix=study-bench
```

JSON 中应满足：

```text
attempted = 23
succeeded = 23
get_attempted + put_attempted = 23
prefill_completed = 7
warmup_completed = 5
attempt_latency_us.samples = 23
```

读写各占多少次不固定为一半：`read_percent=50` 是概率，不是精确配额。
正式计数不包含前面的 7 次预填充和 5 次预热。

将参数改为 `--requests=1 --concurrency=8` 再运行，应看到实际 `concurrency=1`。
这是为了不创建超过请求数量的工作线程。

**注意：** 即使工作负载选 Get，默认预填充仍会覆盖测试前缀下的键；不要对业务库执行。

**看代码：** [bench_main.cpp](../src/bench_main.cpp) 的任务分配、`condition.wait`、`WorkerResult` 和 `join`。

**自问：** 23 次任务为什么分成 6、6、6、5？为什么主线程不在其他线程运行时实时读取普通计数器？

## 实验 7：一键对比同步策略

这个实验自己启动隔离服务，不依赖终端 A。为了减少资源争用，先让终端 A 的手动服务正常退出。
第一次只跑小样本，不把其性能数值当作稳定结论：

```bash
# 在仓库根目录执行
python3 scripts/compare-wal.py \
  --build-dir "$MINIKV_BIN" \
  --output benchmark-results/study-first.json \
  --requests 100 --keys 10 --warmup 10 \
  --concurrency 1 4 --operations put get --repeats 1
python3 -m json.tool benchmark-results/study-first.json
```

预期包含 8 个 case：2 种负载 × 2 档并发 × 2 种同步策略。
输出路径已经存在时会拒绝覆盖，下次使用新的文件名。

先检查成功率和错误数，再看吞吐与 P99。正式比较请使用 [Release 构建与多轮实验命令](benchmarking.md)，
不要把本实验的 Debug、100 次请求结果与已有 Release 样本直接比较。

**看代码：** [compare-wal.py](../scripts/compare-wal.py) 的 `run_case`、`finally`、`summarize`。

**自问：** 为什么每个 case 要新建数据库？为什么关闭同步写带来的提速不是“同样保证下的免费优化”？

## 实验 8：用 GDB 看到真实调用栈

先调试不经过网络的存储测试，减少线程与网络超时干扰。必须使用 Debug 构建：

```bash
gdb --args "$MINIKV_BIN/store_test"
```

进入 `(gdb)` 提示符后逐条执行：

```text
set print thread-events off
break minikv::Store::Put
run
print key
print value
bt
next
disable 1
continue
quit
```

第一次命中 Put 时，预期 key 是 `"key"`，value 是 `"one"`，调用栈包含 Store::Put 和测试程序的 main。
`next` 执行当前源码行而不主动进入被调用函数；`bt` 查看调用栈；`disable 1` 禁用本次新会话中的第一个断点。
`continue` 让测试继续运行到退出，测试自行清理临时数据库。

若提示没有调试符号或值被优化掉，检查是否误用了 `build-release`。
调试真正的 RPC 服务时，断点会暂停处理，请求可能因此超时；不要把这种调试影响直接认定为网络 bug。

## 常见问题

| 现象 | 先检查什么 |
| --- | --- |
| `main.cpp: No such file or directory` | 当前目录与文件名；本项目用 CMake 构建多个源文件，不是任意目录下编译一个 main.cpp |
| `minikv_server: No such file` | `MINIKV_HOME`、构建是否成功，以及实际的 `build-service` 路径 |
| 找不到 `minikv.pb.h` | 是否先通过 CMake 构建，是否误用单文件 g++ 命令 |
| `Address already in use` | 端口已有服务；换空闲端口，不随意结束别人的进程 |
| RocksDB 锁错误 | 是否有另一个进程打开同一 DB；不要删除 LOCK 文件强行绕过 |
| `RPC_ERROR` | 服务是否启动、地址是否一致、超时是否太短 |
| 重启后键不见了 | 是否用了新的临时目录，之前 Put 是否确实成功 |
| benchmark 的 Get 先产生写入 | 默认 `prefill=true`，不是纯只读访问现有业务库的工具 |
| 两个终端的变量不一致 | 每个 Shell 都有自己的环境，要分别设置 |

## 实验 9：TTL 与覆盖写

在前面已启动服务的终端 B，使用同样的 `MINIKV_BIN` 和服务地址；下面显式使用实验端口 18091：

```bash
"$MINIKV_BIN/minikv_cli" --server=127.0.0.1:18091 --ttl_ms=2000 put temporary value
sleep 3
"$MINIKV_BIN/minikv_cli" --server=127.0.0.1:18091 get temporary
echo "exit=$?"
"$MINIKV_BIN/minikv_cli" --server=127.0.0.1:18091 --ttl_ms=2000 put temporary old
"$MINIKV_BIN/minikv_cli" --server=127.0.0.1:18091 put temporary permanent
sleep 3
"$MINIKV_BIN/minikv_cli" --server=127.0.0.1:18091 get temporary
```

第一次 Get 为 `NOT_FOUND`、退出码 2；最后一次为 `permanent`，说明普通 Put 清除了旧 TTL。
这验证逻辑过期，不能证明 SST 空间已经释放。后台删除、旧快照竞争和线程关闭由 `ttl_contract` 自动测试。
阅读 [TTL 设计](ttl-design.md)，重点解释为什么需要“WriteBatch + 同键锁 + 锁内重读”，而不是仅定时 Delete。

## 完成后的整理

在终端 A 用 Ctrl+C 关闭自己启动的服务。手动实验目录会保留供观察；自动化测试和对比脚本的临时数据库会自行清理。
本教程不要求递归删除任何目录，也不要求修改正常服务的数据。

建议记录每个实验的“预期结果、实际结果、对应函数、尚不理解的问题”，再回到
[学习指南](learning-guide.md)的自测题，而不是只记录执行成功的命令。
