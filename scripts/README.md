# 构建、启动与组件测量

这些脚本适配当前 JSON 配置和审计文件保护。使用 Bash；可以从其他工作目录调用脚本的绝对路径，不要求先进入仓库。需要 CMake、Ninja、Python 3；联合启动另需 `ss`（Ubuntu 的 `iproute2` 包）。

## 构建

```bash
# 日常增量构建，默认 Release；默认同时编译程序、测试和基准目标
bash scripts/no_clean_build.sh

# 清理构建目录中的编译产物后重建
bash scripts/build.sh release

# 只构建指定目标，或分别构建 Release / Debug
bash scripts/no_clean_build.sh release exchange_main trading_main
bash scripts/no_clean_build.sh debug
bash scripts/no_clean_build.sh all
```

目录统一为 `build/release`、`build/debug`。Release 默认 `TRADE_PERF_TRACE_EVERY=0`，Debug 默认为 1。两个入口共用实现；`build.sh` 执行 CMake 的 `--clean-first`，`no_clean_build.sh` 保留已有编译产物。均不删除运行日志或审计文件，不自动运行全量测试。

## 启动

```bash
# 单独启动一个客户端；客户 ID、策略、网络和风控来自 config/client.json
bash scripts/run_clients.sh

# 指定另一份客户端配置
bash scripts/run_clients.sh --config /path/client.json

# 联合启动交易所与一个客户端，客户端默认运行 10 秒
bash scripts/run_exchange_and_clients.sh

# 使用 RANDOM 模式生成订单，便于观察订单链；位置参数覆盖 JSON 的 ID 和策略
TRADE_RANDOM_ORDERS=100 bash scripts/run_exchange_and_clients.sh 5 RANDOM
```

联合脚本只增量构建两个主程序，先检查配置，再启动交易所。它通过 `ss` 确认本次交易所 PID 已监听配置端口，再启动客户端；监听检查仅用于协调启动，不代表业务验收。无位置参数时遵循 JSON 策略；示例默认 MAKER 在没有外部订单时可能保持空闲。

每次运行创建新目录并打印路径；联合目录内分开存放 `exchange/`、`client/` 的日志与审计。指定的运行目录必须尚不存在，不覆盖历史文件，也不删除旧审计来绕过重启保护。原脚本中的两组硬编码客户端已取消；需要多个客户端时，分别调用 `run_clients.sh`，使用不同 JSON 客户 ID 和新运行目录。

客户端完成或联合脚本收到 INT/TERM 时，先排空客户端，再停止交易所，只处理本次创建的 PID。失败时保留非零退出码和输出日志。联合脚本默认 10 秒期限；`TRADE_RUN_SECONDS=0` 表示持续运行，可用 Ctrl+C 停止。单独启动客户端不添加期限，使用 JSON 或环境变量的设置。

## 参数

| 环境变量 | 含义 |
|---|---|
| `TRADE_BUILD_DIR` | 单一构建目录；相对路径按仓库根目录解释，不能和 `all` 共用 |
| `TRADE_BUILD_JOBS` | 构建并行数，默认 2 |
| `TRADE_PERF_TRACE_EVERY` | 构建时覆盖诊断采样；组件基准入口固定为 0 |
| `TRADE_SANITIZER` | 构建时选择 `address`、`thread` 或空；普通构建默认空 |
| `TRADE_CLIENT_CONFIG` | 客户端 JSON，默认仓库 `config/client.json`；`--config` 优先 |
| `TRADE_EXCHANGE_CONFIG` | 联合启动的交易所 JSON，默认仓库 `config/exchange.json` |
| `TRADE_RUN_DIR` | 新运行目录；相对路径按调用者当前目录解释，父目录须存在 |
| `TRADE_RUN_SECONDS` | 覆盖客户端运行期限；联合脚本默认 10 秒 |
| `TRADE_RANDOM_ORDERS` / `TRADE_RANDOM_DELAY_US` | 覆盖 RANDOM 数量／间隔，仍由程序校验 |
| `TRADE_SNAPSHOT_MS` | 联合启动的快照周期，默认 200 ms |

自定义 JSON 的相对路径按调用者当前目录解释；脚本会在切换运行目录前转成绝对路径。两端 TCP、组播端点需一致，网卡填写各自本机名称。

## 组件测量

```bash
bash scripts/run_benchmarks.sh
# 或指定尚不存在的输出目录
TRADE_BENCH_ROUNDS=7 bash scripts/run_benchmarks.sh /path/new-component-results
```

入口复用仓库已有的 `release_benchmark`、`logger_benchmark`、`hash_benchmark`、`measurement_component_benchmark`，默认 7 轮；前三项在偶数轮交换内部对照顺序。`hash_benchmark` 是保留的旧目标名，实际比较订单簿实现。各程序保留自身的计数、状态、事件或输出字节核对，失败立即返回非零，不跳过失败结果。

`TRADE_BENCH_BUILD` 可指定构建目录，`TRADE_BENCH_ROUNDS` 可指定正整数轮数。此入口固定使用 Release、诊断计时 0、无 sanitizer。输出按轮次／程序分目录，保存原始结果、stderr、文件日志、构建输出和环境；已有输出目录拒绝复用。

**这里只测当前组件，不测 TCP 订单往返，也不自动生成旧／新源码提速结论。** `OptMemPool`、`OptLogger` 是当前实现的别名，内部两组属于重复对照；完整性能结论仍应按 [07 报告](../record/07_book_recovery.md) 的范围和配对方法阅读。原入口调用的历史 Python 批次驱动不再随仓库提供，因此移除了失效路径。
