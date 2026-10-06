# TradeSystem｜C++20 低延迟交易系统

TradeSystem 是一个面向 Linux 的 C++20 模拟交易系统，由模拟交易所和交易客户端两个进程组成。交易所负责接单、定序与撮合，客户端负责行情恢复、策略决策、订单管理与风险控制。订单与回报走 TCP，增量行情与快照走 UDP 组播，组件之间通过有界 SPSC 队列交接。

本仓库围绕消息完整性、订单状态、可控停机和可复现性能测量持续改造。每项工作都保存问题复现、修复代码、验证方法与实测结果，方便检查实现是否正确、性能变化来自哪里。

## 系统总览：订单与回报，行情与恢复

![TradeSystem 系统总览：蓝色 TCP 订单与回报，橙色 UDP 增量与快照](assets/architecture/system-overview-v2.png)

蓝色表示订单与私有回报，橙色表示公开行情。两个进程通过 TCP 和 UDP 组播通信，进程内的跨线程消息由有界 SPSC 队列交接。图中的框表示模块职责；交易引擎内的特征、策略和订单管理在所属线程中执行。

**订单闭环。** 策略或随机输入产生请求，统一发单口完成校验与风险预占，订单网关经 TCP 发送到交易所。订单服务器解析和校验请求，FIFO 定序器交给撮合引擎；受理、撤单和成交回报沿 TCP 返回客户端。`ACCEPTED` 表示受理，`FILLED` 表示成交；持仓按成交回报更新。

**行情闭环。** 撮合产生市场更新，发布器添加增量序号，并把同序号副本交给快照合成器。客户端正常消费增量，发现缺口时用完整快照衔接连续增量；重建本地簿后核验哈希与恢复代次，确认可信后放行新单。

### 三份状态各自负责什么

| 状态 | 负责什么 | 更新来源 |
| --- | --- | --- |
| 交易所中心簿 `MEOrderBook` | 保存活动订单，决定模拟撮合结果 | 新单、撤单等合法请求 |
| 快照合成器的活动订单集合 | 保存某个增量水位上的副本，发布恢复快照 | 撮合产生的增量更新 |
| 客户端本地簿 `MarketOrderBook` | 维护策略看到的盘口与 BBO | 增量行情或完整恢复批次 |

## 订单簿：索引找节点，链表维护顺序

![中心订单簿的数据结构：完整价档索引、双向循环链表、同价 FIFO、撤单索引和对象池](assets/architecture/order-book-design.png)

撮合引擎为每个品种维护一份中心簿。其结构分为四层：

1. **完整价档索引。** 主实现用 `std::map<PriceKey, MEOrdersAtPrice*>`，键为 `(Side, Price)`。买卖方向和完整价格共同标识价档，价格 100 与 356 可以分别定位。
2. **按价格排列的价档链。** 买侧从高价到低价，卖侧从低价到高价；双向循环链表配合最佳买价、最佳卖价指针，使撮合从当前最优档开始。
3. **同价 FIFO 与撤单索引。** 每个价档内的订单也是双向循环链表，新单追加到尾部，撮合从队首消费。订单节点自带前驱、后继指针，索引与链表指向同一份订单。每个客户端另有 `OrderId → MEOrder*` 的稀疏映射，撤单先定位节点，再把它从链中摘除。
4. **固定容量的对象池。** 订单节点和价档节点分别由 `MemPool` 管理；空闲槽栈提供可复用位置，分配时构造对象，归还时析构对象。

图中的示意队列为 A → B → C。撤掉 B 时，索引找到 B，连接 A 与 C，再回收 B 的槽；随后加入 D，队列成为 A → C → D，原有订单的先后顺序保持。

**这套设计的代价。** 主实现的价档索引查找为 O(log P)，其中 P 是活动价档数；插入新价档仍需沿链寻找位置，索引容器也可能动态分配内存。对象池减少订单和价档节点的分配，整体耗时仍由负载与测量决定。仓库还保留以完整价格键建立 `unordered_map` 索引的对照实现，性能比较见 [03 调优记录](record/03_performance_tuning/03_performance_tuning.md)。

实现入口：[节点与索引类型](exchange/matcher/me_order.h)、[中心簿](exchange/matcher/me_order_book.h)、[撮合处理](exchange/matcher/me_order_book.cpp)、[对象池](common/mem_pool.h)。

## 交易客户端：行情驱动决策，回报确认状态

![客户端内部回路：本地簿、特征、策略、统一发单口、订单网关及回报更新](assets/architecture/trading-client-architecture-v2.png)

`MAKER` 和 `TAKER` 根据本地簿及特征计算订单目标，由 `OrderManager` 管理策略订单。`RANDOM` 直接调用统一发单口；最终出口都执行字段、会话与风险检查，新单还要求行情可信。请求无法进入队列时，已预占的风险额度回滚。

回报进入 `TradeEngine::onOrderUpdate()` 后，先做身份、重复回报和风险状态检查；`FILLED` 更新持仓与盈亏，策略回调更新订单状态。行情缺口会使本地状态失效，恢复批次一次入队，并由交易引擎核验重建结果，随后恢复新单准入。

实现入口：[交易引擎](trading/strategy/trade_engine.cpp)、[订单管理](trading/strategy/order_manager.h)、[风险控制](trading/strategy/risk_manager.h)、[行情恢复](trading/market_data/market_data_consumer.cpp)。

## 其他值得关注的设计

| 设计 | 解决的问题 | 实现与边界 |
| --- | --- | --- |
| 有界 SPSC 消息队列 | 跨线程交接与容量约束 | acquire/release 交接槽位所有权，满队列返回失败；适用于单生产者、单消费者。见 [LFQueue](common/lf_queue.h)。 |
| 保留 TCP 发送进度 | 短写或暂时不可写时保存尾部 | 按实际发送字节推进，完整帧进入缓冲后才出队。见 [TCPSocket](common/tcp_socket.cpp)。 |
| 完整批次的行情恢复 | 避免策略使用部分重建的盘口 | 检查周期、水位、序号与哈希；本地核验后恢复新单。见 [恢复验收](record/01_reliability/01_reliability.md)。 |
| 线程所有权与排空 | 让停止和资源释放有顺序 | 停新输入、按依赖排空、join 工作线程。见 [线程工具](common/thread_utils.h)及 [可靠性记录](record/01_reliability/01_reliability.md)。 |
| 关键审计与诊断日志分开 | 使交易事件可对账，性能观测有清晰范围 | 关键审计逐条持久化，工具支持只读对账；自动崩溃重放尚未实现。见 [可靠性记录](record/01_reliability/01_reliability.md)。 |
| 逐请求的外部测量 | 把生成器迟到、请求往返和窗口吞吐分开 | 固定输入、多轮测量、保留慢轮与未完成请求。见 [测量口径](record/02_reproducible_measurement/02_reproducible_measurement.md)。 |

## 技术栈 / Tech Stack

| 领域 | 本项目中的实现 |
| --- | --- |
| 语言与构建 | C++20、GCC、CMake、Ninja；Linux 与 `pthread` |
| 网络通信 | TCP 订单与回报、`epoll` 事件循环、UDP 组播增量与快照 |
| 并发交接 | 组件工作线程、有界 SPSC 队列、原子读写水位与 CPU 亲和性 |
| 订单簿与内存 | 完整价格键、稀疏订单索引、双向循环链表、固定容量对象池 |
| 交易逻辑 | 请求定序、价格优先与同价 FIFO、订单状态、风险预占、持仓与盈亏 |
| 验证与测量 | 参考模型、故障注入、ASan/UBSan、TSan、固定输入与逐请求延迟样本 |

## 已完成的改造

- [组播接口选择](record/00_multicast_interface/00_multicast_interface.md)：明确收发接口，验证增量与快照组播路径。
- [可靠性改造](record/01_reliability/01_reliability.md)：有界队列、TCP 背压与部分发送、完整价格键、协议与会话校验、在途风控、行情恢复、线程排空及持久审计。
- [可复现测量](record/02_reproducible_measurement/02_reproducible_measurement.md)：修正基准计数与计时口径，核对业务输出，保存固定输入和逐条请求样本，分别运行开环、闭环与容量实验。

这是模拟环境。持久审计支持事后对账；已有审计文件会阻止直接复用该运行目录，进程崩溃后的自动重放和继续交易尚未实现。

## 目录 / Repository Layout

```text
common/       线程、网络、队列、内存池、日志和性能工具
exchange/     订单服务器、撮合引擎、行情发布与快照合成
trading/      行情接收、订单网关、交易引擎和示例策略
benchmarks/   微基准程序
notebooks/    已清除运行输出的性能分析 notebook
scripts/      构建与示例运行脚本
assets/       系统总览、订单簿与客户端设计图
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

2026-10-06 新增[perf重新测量报告](record/05_perf_remeasurement/README.md)：实际采集软件事件与调用栈，完成队列索引/布局/批量对照和独立容量验收。保留逐记录fsync的loopback NEW/CANCEL场景下，50请求/秒通过五个64秒窗口（P99中位15.649 ms）及追加304秒观察（P99 18.156 ms）；100请求/秒未通过独立容量验收。该档位不是生产最大容量，组件收益不等于业务收益。05采用固定提交加归档冻结补丁复现，公开报告与数值汇总，默认实现不随实验结果自动切换。

## 来源与许可 / Attribution

项目的初始代码来源于 Packt Publishing《[Building Low Latency Applications with C++](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP)》。本仓库在其基础上进行改造，遵循 [MIT License](LICENSE)，保留原版权声明；后续改动与验证记录见 `record/`。
