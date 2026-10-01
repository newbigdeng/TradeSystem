# TradeSystem｜C++ 低延迟交易系统

TradeSystem 是一个运行于 Linux 的 C++20 模拟交易系统，包含交易所撮合引擎、订单网关、行情发布与订阅、交易策略和风险控制。订单经 TCP 传输，增量行情与快照经 UDP 组播分发，客户端维护本地订单簿并处理行情恢复。

本仓库围绕消息完整性、订单状态、可控停机和可复现性能测量持续改造。每项工作都保存问题复现、修复代码、验证方法与实测结果，方便检查实现是否正确、性能变化来自哪里。

## 已完成的改造

- [组播接口选择](record/00_multicast_interface/00_multicast_interface.md)：明确收发接口，验证增量与快照组播路径。
- [可靠性改造](record/01_reliability/01_reliability.md)：有界队列、TCP 背压与部分发送、完整价格键、协议与会话校验、在途风控、行情恢复、线程排空及持久审计。
- [可复现测量](record/02_reproducible_measurement/02_reproducible_measurement.md)：修正基准计数与计时口径，核对业务输出，保存固定输入和逐条请求样本，分别运行开环、闭环与容量实验。

这是模拟环境。持久审计支持事后对账；已有审计文件会阻止直接复用该运行目录，进程崩溃后的自动重放和继续交易尚未实现。

## 系统架构 / Architecture

![模拟交易所、客户端与订单/行情通道的系统总览](assets/architecture/system-overview.png)

**订单链路 / Order path：** 客户端交易引擎产生请求，经订单网关通过 TCP 发送到交易所订单服务器；请求经 FIFO 定序后进入撮合引擎，订单响应再经 TCP 返回客户端。

**行情链路 / Market-data path：** 撮合引擎产生市场更新；行情发布器通过 UDP 多播发送增量行情，并由快照合成器发布订单簿快照。客户端行情接收器在正常状态消费增量数据，在序号出现缺口时利用快照恢复同步。

### 模拟交易所 / Exchange

![订单服务器、撮合引擎与行情发布器的内部架构](assets/architecture/exchange-architecture.png)

- **订单服务器 / Order Server：** 基于 TCP 与 `epoll` 处理连接和订单，FIFO 定序器把请求交给撮合引擎。
- **撮合引擎 / Matching Engine：** 按订单簿状态处理新增、撤单与撮合，生成客户端回报和市场更新。
- **行情发布 / Market Data Publisher：** 发布增量消息，快照合成器维护并发布周期性快照。

### 交易客户端 / Trading Client

![行情接收、本地订单簿、交易引擎与订单网关的内部架构](assets/architecture/trading-client-architecture.png)

- **行情接收 / Market Data Consumer：** 接收增量与快照多播消息，处理序号缺口与快照同步。
- **交易引擎 / Trade Engine：** 更新本地订单簿，组织特征计算、策略、风险检查、订单管理和持仓跟踪。
- **订单网关 / Order Gateway：** 通过 TCP 发送订单并接收回报；示例策略包括做市、流动性获取和随机订单生成。

## 技术栈 / Tech Stack

| 领域 | 本项目中的实现 |
| --- | --- |
| 语言与构建 | C++20、GCC、CMake；Linux 环境，使用 `pthread` |
| 网络编程 | TCP 订单通道、`epoll` 事件循环、UDP 多播增量行情与快照 |
| 并发通信 | 多线程组件、基于原子操作的预分配环形队列、CPU 亲和性设置 |
| 数据结构 | 交易所订单簿与客户端本地订单簿、价格档位和订单索引、对象内存池 |
| 交易逻辑 | FIFO 请求定序、撮合、订单管理、风险检查、持仓跟踪、做市与流动性获取示例 |
| 性能分析 | 时间戳埋点、日志与队列/内存池/哈希等基准程序；性能结论需以可复现实测为准 |

## 目录 / Repository Layout

```text
common/       线程、网络、队列、内存池、日志和性能工具
exchange/     订单服务器、撮合引擎、行情发布与快照合成
trading/      行情接收、订单网关、交易引擎和示例策略
benchmarks/   微基准程序
notebooks/    已清除运行输出的性能分析 notebook
scripts/      构建与示例运行脚本
assets/       重新绘制的架构图
record/       按编号归档的报告、测试和测量工具
```

## 构建与运行 / Build & Run

在 Linux 上安装 GCC、CMake、Ninja 和 Python 3。项目的网络地址与接口目前在 `exchange/exchange_main.cpp` 和 `trading/trading_main.cpp` 中设为本机回环接口 `lo`；跨机器运行前需要调整它们。

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j 4
ctest --test-dir build/release --output-on-failure

# 终端 1：启动模拟交易所
exchange_binary="$(realpath build/release/exchange_main)"
run_dir="$(mktemp -d)"
(cd "$run_dir" && TRADE_SNAPSHOT_MS=200 "$exchange_binary")

# 终端 2：启动随机订单客户端
trading_binary="$(realpath build/release/trading_main)"
run_dir="$(mktemp -d)"
(cd "$run_dir" && TRADE_RUN_SECONDS=10 TRADE_RANDOM_ORDERS=100 "$trading_binary" 5 RANDOM)
```

项目也提供 `scripts/` 下的示例脚本。运行过程中产生的 `*.log`、构建目录和 HTML 分析报告不会提交到 Git。

每次运行使用独立目录保存日志与审计文件。客户端完成初始快照验证后才开始交易；上例将快照间隔设为 200 ms，便于快速演示。结束时使用 `Ctrl+C` 触发排空和停机；历史审计文件应先完成对账，再由操作者归档。

## 性能测量

```bash
python3 record/02_reproducible_measurement/run_measurements.py \
  --build build/release --output "$HOME/trade-measurements/new-batch"
```

正式测量要求源码已提交、Release 构建且结果目录不存在。默认预热一次、独立测量五次，保留每轮的吞吐、P50/P95/P99、最大值、请求完整性和资源记录。测量使用独立负载生成器；具体边界、环境和结果见 [02 测量报告](record/02_reproducible_measurement/02_reproducible_measurement.md)。

## 来源与许可 / Attribution

项目的初始代码来源于 Packt Publishing《[Building Low Latency Applications with C++](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP)》。本仓库在其基础上进行改造，遵循 [MIT License](LICENSE)，保留原版权声明；后续改动与验证记录见 `record/`。
