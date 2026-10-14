# 08｜构建与运行脚本适配

验证日期：2026-10-14。环境：Ubuntu 26.04、GCC 15.2、Bash、CMake、Ninja。程序基线为 `18994a9`，本次调整脚本和说明。

**结论：旧脚本与当前构建说明、JSON 配置和审计保护不一致。现已统一构建入口，运行记录按次隔离，退出只管理本次启动的进程；失效的测量入口改为调用现有组件基准。定向构建、两次订单对账、异常退出及 28 次组件运行通过。**

## 测试时发现的问题

| 文件 | 旧行为与问题 | 当前处理 |
|---|---|---|
| `build.sh`、`no_clean_build.sh` | 使用 `cmake-build-release/debug`，依赖当前目录；两份构建逻辑重复，和 README 的 `build/release` 不一致 | 共用实现，统一 `build/release/debug`，可从外部目录调用 |
| `run_clients.sh` | 脚本硬编码 MAKER 参数及 RANDOM 客户端，位置参数覆盖 JSON 的策略和风控 | 默认启动一个 JSON 配置的客户端，保留显式覆盖入口 |
| `run_exchange_and_clients.sh` | 总是清理重建，固定等 10 秒；进程共用调用目录，用名称 `pkill` 停机 | 增量构建主程序，确认本次 PID 监听；日志和审计分目录，按 PID 收尾 |
| `run_benchmarks.sh` | 调用已经不随仓库提供的 `record/02_reproducible_measurement/run_measurements.py` | 直接复用已有四个组件基准，保留逐轮输出和失败退出 |

旧式客户端参数仍被主程序支持，所以不是参数协议失效；问题是脚本中的值继续覆盖 JSON，修改配置后不容易得到预期行为。旧构建脚本与旧启动脚本配套使用也能构建，但不能直接接上 README 的新构建目录。

## 寻找问题

逐项对照当前 main 的 `--config`、`--check-config` 和位置参数解析，再核对 CMake 复制配置的目录，以及日志／审计的创建方式。

有两个细节直接影响运行：

- **配置路径要先转成绝对路径。** 启动前切换到独立运行目录，相对 JSON 路径若没先处理，就可能指向错误文件。构建路径则统一按仓库根目录解释。
- **审计文件不能覆盖。** 主程序使用 `O_EXCL` 创建审计，已有文件会阻止再次启动。脚本应创建新目录并保留旧记录，不能通过删日志规避保护；普通日志也不应被下一次运行覆盖。

监听检查还必须包含本次 PID。端口已被旧进程占用时，单看“端口可连接”可能误认为新交易所启动成功。当前脚本用 `ss` 检查本次 PID 的监听状态；这是启动协调，业务正确性仍由订单对账检查。

## 解决问题

### 统一构建

文件：[build.sh](../scripts/build.sh)、[no_clean_build.sh](../scripts/no_clean_build.sh)、[common.sh](../scripts/common.sh)。旧脚本分别维护下面这类调用：

```bash
cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B ./cmake-build-release
cmake --build ./cmake-build-release --target all -j 4
```

现在只保留一份配置、构建实现，关键调用为：

```bash
cmake -S "$project_root" -B "$directory" -G Ninja -DCMAKE_BUILD_TYPE="$type" \
  -DTRADE_PERF_TRACE_EVERY="$trace" -DTRADE_SANITIZER="${TRADE_SANITIZER:-}" \
  -DBUILD_TESTING=ON
```

默认 Release、并行数 2；可选择 debug、all 或指定目标。Release 诊断采样默认为 0，Debug 为 1。`build.sh` 用 `--clean-first` 保留清理重建入口，`no_clean_build.sh` 复用同一实现并关闭清理。联合启动只增量构建两个主程序。

### 配置与进程收尾

文件：[run_clients.sh](../scripts/run_clients.sh)、[run_exchange_and_clients.sh](../scripts/run_exchange_and_clients.sh)。原来直接运行硬编码命令，并按名字停止交易所：

```bash
./cmake-build-release/trading_main 5 RANDOM &
pkill -2 exchange
```

客户端现在用数组保存完整命令，配置和路径带空格时也保持为一个参数：

```bash
client_command=("$client_binary" --config "$client_config" "$@")
exec "${client_command[@]}"
```

`exec` 让保存的 PID 对应实际程序。联合启动分别记录交易所、客户端 PID，正常结束或收到 INT/TERM 时先通知客户端排空，再停止交易所；不再按进程名称匹配。客户端失败保留其退出码。单独客户端读取 JSON 的期限；联合脚本默认运行 10 秒，也可用环境变量覆盖。

运行目录每次新建。即使指定 `TRADE_RUN_DIR`，已存在的目录也拒绝复用。默认 JSON 的 MAKER 在空簿、无外部订单时可能空闲；生成演示订单时可以明确传 `5 RANDOM`。多客户端分别使用不同 ID 的配置和不同运行目录。

### 修复组件测量入口

文件：[run_benchmarks.sh](../scripts/run_benchmarks.sh)。旧入口依赖已经移走的驱动：

```bash
python3 record/02_reproducible_measurement/run_measurements.py \
  --build "$build_directory" --output "$output_directory"
```

现在增量构建并直接调用现有四个目标：`release_benchmark`、`logger_benchmark`、`hash_benchmark`、`measurement_component_benchmark`。默认 7 轮，前三项在偶数轮交换内部对照顺序；逐轮保存结果、stderr、日志和环境，任何程序失败立即停止并返回非零。

`hash_benchmark` 是旧目标名，实际比较订单簿；`OptMemPool` 和 `OptLogger` 是当前实现别名，属于重复对照。**这个脚本只测当前组件，不自动重现历史批次、TCP 往返或旧／新源码比较。** 结果范围必须按程序输出阅读。

## 重新测试

21 项定向检查完成：

- 六个 shell 文件通过 `bash -n`；非法构建模式、并行数及 all 共用目录被拒绝。
- 从带空格的外部目录完成 Release 全目标增量构建、Debug 指定目标清理构建，并运行 Debug 容量契约。缓存分别确认构建类型和诊断采样设置。
- 带空格的自定义 JSON 路径正确读出客户 7、TAKER；非法策略在启动子进程前返回 2。
- 两次独立联合运行均返回 0。每次生成 12 条新单、12 条随机撤单，停机另有 4 条自动撤单；审计各为 **28 RECEIVED、28 APPLY、30 RESPONSE**。RECEIVED 与 APPLY 字段、顺序逐项一致，结束时客户端和交易所三个队列均为 0，会话健康为 1。
- INT/TERM 中实际验证 TERM：联合脚本返回 143，两个程序均正常排空；另一个同名进程继续存活。INT 使用相同的收尾函数，未单独注入验证。
- 端口被其他进程占用时返回失败，不启动客户端交易；重复运行目录被拒绝，原运行文件按字节保持不变。
- 默认 7 轮 × 4 程序，共 **28 次组件运行**成功；结果文件完整、stderr 为空，日志基准输出逐字节核对并确认零丢弃。基准缓存确认 Release、诊断采样 0、无 sanitizer。

验证范围是脚本兼容性、真实订单与收尾行为。本次没有改 C++ 热路径，性能结论继续按 [07 报告](07_book_recovery.md) 阅读。测量原始文件和验收驱动保留在独立实验归档中。

直接使用方法见 [scripts/README.md](../scripts/README.md)，根 README 的快捷命令也已更新。
