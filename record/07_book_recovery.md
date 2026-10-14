# 07｜行情恢复、策略配置与消费热路径优化

本轮验证：2026-10-13 至 2026-10-14，Ubuntu 26.04、GCC 15.2、C++20、Release。基线是开始时保存的完整工作区，包含此前尚未提交的改造；它不是只检出 `f52d1f0` 就能还原的版本。

**结论：队列和消费组件减少了处理开销，业务核对通过；本轮订单网络往返 P50 没有改善，不能称为整链提速。采用简单队列、失败时才生成动态错误文本和单条跳过排序，保留原对象池及 Logger。**

## 测试时发现的问题

- 队列已经用 N−1 掩码定位槽位，但游标回绕和距离仍带条件分支；固定容量只在运行时检查。
- 快照缓存 ADD、MODIFY、CANCEL 的检查消息包含 `toString()`。检查成功也会先拼接文本，正常路径做了无用工作。订单回报的缺连接检查也提前拼接客户 ID。
- 定序器对一条请求仍调用 `stable_sort`。本机 GCC 15 实现会为非空区间申请临时缓冲，单条没有排序需求。
- 对象池拆分标记看起来能省内存，但需要实测是否更快。普通日志的流字符串也存在可独立比较的复制开销。

## 寻找问题

本轮逐项比较，先保存基线，再只改变一个因素，验证消息与对象状态，最后测性能。队列累计候选还进行了同窗三版本对照，避免把不同时间的快慢误当成某个缓存的贡献。

快照开销主要来自函数参数提前求值。当前 `ASSERT` 接收 `string_view`，纯字面量没有临时字符串分配；但 `"Received:" + update.toString()` 是动态表达式，调用 ASSERT 前已经执行，不能靠 ASSERT 内部的失败分支省掉。

消费速度还受同步审计限制：网络线程接收请求后写入 RECEIVED，撮合写 APPLY、RESPONSE，仍逐条 `fsync`。网络线程的一轮接收可能先处理多条请求，再消费已有回报。提高纳秒或微秒级组件速度，不能保证消除毫秒级刷盘和排队。这一轮保留原持久化边界，没有把审计改成无确认后台任务。

## 解决问题

### 1. 常量容量用 concept，运行时容量仍检查

文件：[lf_queue.h](../common/lf_queue.h)。原调用 `ClientRequestLFQueue requests(ME_MAX_CLIENT_UPDATES);` 现在使用标签：

```cpp
template <size_t N>
concept PowerOfTwoQueueCapacity = std::has_single_bit(N) && N <= SIZE_MAX / 2;

template <size_t N> requires PowerOfTwoQueueCapacity<N>
struct QueueCapacity final {};

ClientRequestLFQueue requests(Common::QueueCapacity<Common::ME_MAX_CLIENT_UPDATES>{});
```

`QueueCapacity<3>` 编译不通过；运行时整数仍可传给原构造函数，由它抛出 `invalid_argument`。两个 main 的六个业务队列和 publisher 的内部队列均用标签，容量数值未改。Logger 可自定义运行时容量，保留原入口；没有把大队列搬到线程栈。

### 2. 槽位和游标用不同掩码

原来 `next` 到 2N 时分支归零，`distance` 按 tail 是否落后 head 分支计算。现在：

```cpp
size_t next(size_t i) const noexcept { return (i + 1) & cursor_mask_; }
size_t distance(size_t tail, size_t head) const noexcept {
  return (tail - head) & cursor_mask_;
}
// mask_=N-1；cursor_mask_=2*N-1。
```

容量 4 写满时 head=0、tail=4，距离必须是 4。若距离错误地也用 `& 3`，就会得到 0，把满队列当空。两圈游标、全部 N 个槽可用、acquire/release、失败零发布均保留。

空批次直接成功返回。`full_count` 只有生产端写，改为 relaxed load 加一后 store，读者仍读取原子值；Logger 多个提交线程的整次生产操作仍由 mutex 串行化。该写法不能用于多个并发写者的计数器。没有保留消费者 tail 缓存或额外高水位缓存。

### 3. 失败时才生成动态错误文本

文件：[snapshot_synthesizer.cpp](../exchange/market_data/snapshot_synthesizer.cpp)、[order_server.h](../exchange/order_server/order_server.h)。例如原 ADD 检查：

```cpp
ASSERT(order == nullptr, "Received:" + me_market_update.toString() +
                             " but order already exists:" + (order ? order->toString() : ""));
```

改为：

```cpp
if (UNLIKELY(order != nullptr))
  ASSERT(false, "Received:" + me_market_update.toString() +
                    " but order already exists:" + order->toString());
```

MODIFY、CANCEL 缺订单的诊断同样延后，回报消费先保存 socket 指针，只在缺连接时拼接客户 ID。范围、方向、序号、重复订单、发送缓冲满时不 pop 等检查仍在。另删除撮合分支不可达的第二个 `break`，不宣称它带来可测提速。

### 4. 一条请求不用排序

文件：[fifo_sequencer.h](../exchange/order_server/fifo_sequencer.h)。原来对所有非空集合排序，现在仅在 `pending_size_ > 1` 时执行原 `stable_sort`。多条请求仍按接收时间稳定排序；队满时只移走已经成功发布的 i 条，`pending_size_ -= i` 保留失败项，i=0 不做重叠搬移。

### 5. 交易引擎的品种诊断也只在失败时生成

文件：[trade_engine.cpp](../trading/strategy/trade_engine.cpp)。原来每条普通行情还会为品种检查重复格式化一次：

```cpp
ASSERT(market_update->ticker_id_ < ticker_order_book_.size(),
       "Unknown ticker-id on update:" + market_update->toString());
```

现在错误时才生成相同诊断：

```cpp
if (UNLIKELY(market_update->ticker_id_ >= ticker_order_book_.size()))
  ASSERT(false, "Unknown ticker-id on update:" + market_update->toString());
```

RECOVERY_COMMIT 仍先处理，旧标记仍忽略；合法更新正常进入订单簿，非法品种仍在下标访问前失败。

### 未采用的候选

- **队列游标与高水位缓存：**同窗直接比较没有优势，增加的状态不保留。
- **对象池标记分离：**五种真实节点每 2^20 槽（约一百万）净省 7 MiB 数组，但单槽借出、归还全部 7/7 轮变慢；MarketOrder 配对中位耗时增加 17.70%。保留原 `Block.storage + used`、uint32 空闲编号、O(1) LIFO、对齐与释放检查。随机 65,536 槽是池压力场景，尤其价档实际容量只有 256，不能把压力结果当成正式价档吞吐。
- **删池检查、先弹空闲编号再构造：**不采用。前者失去非法释放保护；后者在构造抛异常时会丢空闲槽。非平凡析构也必须保留。
- **替换成自旋锁或无确认异步审计：**不能缩短磁盘刷盘，且会改变故障处理与持久化保证，未混入本轮。
- **移动日志流字符串：**`std::move(text).str()` 单独测了 7 对，回报消费配对 P50 增加 0.93%，批量定序减少 0.87%，快慢与尾延迟没有一致收益；保留原写法。

## 重新测试

### 业务与内存检查

当前 README 的 Release 全目标构建通过，两份 JSON 主程序检查模式通过。第一轮合并运行 15 项相关 Release 用例、7 项 ASan/UBSan 用例全部通过；最后的交易引擎改动仅复测新增消费契约、策略行为、恢复应用 3 项，Release 与 ASan/UBSan 均通过，没有在小改后重跑全量回归。

检查包括百万消息 FIFO 与载荷完整性、容量 1 和多次回绕、整批失败零发布、背压后缀保留、回报字段／epoch／顺序、快照对象状态与失败诊断、真实交易引擎的 BBO／特征／回调／对象池／恢复代次，以及非法品种仍失败。所有 pragma region 原文保留，生产对象池与 Logger 均未混入未采用候选。

### 队列组件（2026-10-13）

固定输入、7 轮同窗旧版／缓存版／简化版对照。下表是整窗平均成本的轮间中位数；不是单消息 P50。

| 场景 | 旧版 → 简化版 | 配对成本变化 | 更快对数 |
|---|---:|---:|---:|
| 单条入出队，ns/消息 | 9.711 → 8.239 | -15.08% | 6/7 |
| 批量，ns/消息 | 8.488 → 6.813 | -19.78% | 7/7 |
| 跨核，ns/消息 | 92.789 → 88.360 | -6.10% | 5/7 |
| 满队失败，ns/调用 | 14.452 → 2.777 | -79.87% | 7/7 |

跨核有两对变慢，不能宣布稳定跨核提速。满队失败统计的收益只针对队满场景，不能拿它代表正常订单处理。缓存组合相对简化版，单条配对反而增加 10.25%、批量增加 5.84%，因此不保留缓存。

### 消费组件（2026-10-14）

前三个场景是每批耗时除以批大小后统计的 P50/P99，再取 7 轮中位数。各版样本数自适应，累计计时至少 200 ms；普通文件日志均保留，84 个正式样本和 120 份已结束日志均核对，日志零丢弃。计时外的队列装填与日志排空不计入这一指标。

| 单因素场景 | P50 原版 → 新版，ns/项 | P99 原版 → 新版，ns/项 | 配对 P50 变化 | 更快对数 |
|---|---:|---:|---:|---:|
| 快照缓存 ADD/MODIFY/CANCEL | 1423.05 → 16.34 | 1992.04 → 68.73 | -98.85% | 7/7 |
| 回报消费、编码与缓冲复制 | 6769.70 → 6554.69 | 13425.40 → 12998.10 | -4.22% | 5/7 |
| 单条定序 | 24925.00 → 24649.00 | 136644.00 → 139131.00 | -1.07% | 5/7 |

快照的 98.85% 只指缓存状态应用，不含周期快照生成／发送、普通 run 日志、UDP 或恢复整链。回报模式不含实际 send、撮合或关键审计刷盘。单条定序仅小幅改善 P50，P99 上升；64 条批次仍调用稳定排序，批次差异不归因于跳过单条排序。

真实 `TradeEngine::run` 另测：每批 96 个 ADD/MODIFY/CANCEL，进入本地簿、特征和观察策略虚回调。每轮固定 15,936 计时事件，另有 32 批预热，14/14 日志零丢弃，最短计时 410.42 ms。整窗平均成本的 7 轮中位数 **31.229 → 28.1525 µs/事件**，配对降低 **4.31%**，5/7 更快；两对回退保留。批均值 P50 为 29.116 → 27.223 µs，P99 为 55.066 → 54.4437 µs，只有 4/7 对 P99 降低，不能声称尾延迟稳定改善。此范围不含网关、UDP/TCP 或关键审计。

### 交易所订单网络往返（2026-10-14）

相同的 4 客户、8 品种、seed=20260928、128 条预热和 256 条正式请求，旧、新各 7 轮，交替运行；两种输入速率分别比较。关键审计仍逐条 fsync，普通日志保留，旧版文件名前缀归一化到当前源码路径，避免仅因日志路径更长而增加工作。

计时从外部生成器实际发出完整请求帧，到收到并核对 ACCEPTED/CANCELED。范围包括交易所 TCP、定序、撮合及回报，客户端策略、网关和行情接收不在这个指标中。

| 速率 | P50 旧 → 新，ms | P99 旧 → 新，ms | 配对 P50 变化 | 新版更快对数 |
|---|---:|---:|---:|---:|
| 50/s | 9.826 → 10.417 | 17.890 → 15.633 | +7.57% | 2/7 |
| 500/s | 1382.838 → 1496.510 | 1928.509 → 2119.972 | +13.56% | 2/7 |

28 轮全部完成业务对账，输入字节一致。表中两组分位数的中位数与逐对比例中位数是不同统计，方向可能不同，不能只选更好看的一个。50/s 时三条队列高水位都为 1，往返仍约 10 ms，延迟无法用队列积压解释。500/s 时，0.512 秒窗口结束后两版未回请求中位数都是 252 条；后续全部交付需约 2.407 → 2.599 秒。请求队列最高 249 条，容量 262144 条，增加容量不能提高消费速度。

两版每轮 RECEIVED、APPLY、RESPONSE 各 384 条，合计 1153 条审计记录含头部。持久化次数与主流程没有增加，write/fsync 仍共用审计锁。网络线程先接收并逐条做 RECEIVED 同步审计，轮末定序后才消费回报，已产生的回报有等待机会；本轮没有测出这些等待各占多少。

同一个当前二进制另做 500/s 的 7 对对照，14 轮均正确。仅重复运行，P50 配对中位就相差 −8.30%，各对为 −38.06%～+17.52%，说明运行波动明显。这不能证明本次源码没有回退，也不能证明物理主机性能下降。缺少逐次 fsync、锁等待和宿主机同步记录，新增延迟的具体分量尚未定位；当前结论是组件优化有效，网络整体提速未验证成功。


定向测试、基准和旧队列头文件保存在 [07_queue_pool_consumer](07_queue_pool_consumer/)。README 使用当前 C++20、离线 Glaze、Release trace=0 和 JSON 启动方式。完整原始轮次保留，未因变慢或尾延迟上升而删除样本。

<details>
<summary>此前的行情恢复、策略配置与价格档改造：原／新代码和历史测量</summary>


验证日期：2026-10-13。环境：Ubuntu 26.04、GCC 15、C++20、Release，关闭诊断计时采样。基线采用本次工作开始前保存的实际工作区：HEAD 为 `f52d1f0`，包含此前尚未提交的价格档与恢复改动。

这次主要完成四件事：明确快照恢复流程并修复两个通道边界；把策略统一为基类接口；用两份 JSON 配置网络和策略；整理特征引擎，精简客户端及备用订单簿的价档插入。

## 测试时发现的问题

- 实时增量正常时，合法周期快照会被忽略，但损坏快照在进入处理函数前就触发了恢复。没有必要让未使用的周期快照打断正常增量。
- 增量接收入口未限制通道内容，快照开始、结束帧可能进入本地订单簿。控制帧没有普通品种信息，继续处理可能触发非法品种断言。
- 策略选择靠三组回调绑定；网络端点直接写在两个主程序中，改部署地址需要改源码。
- 客户端和备用对照簿仍有重复的买卖分支、两套链表插入以及已知非空后的重复判空。特征计算也缺少空行和公式说明。

前两个问题均有旧版失败、新版通过的确定性测试，不是只观察程序能启动。

## 寻找问题

### 快照到底怎样恢复

入口是 [MarketDataConsumer](../trading/market_data/market_data_consumer.cpp)，拼接状态由 [MarketRecovery](../trading/market_data/market_recovery.h) 管理。

1. **启动先恢复。** 本地没有可靠盘口，不能直接使用收到的第一条增量。
2. **正常运行只接连续增量。** 旧重复序号忽略；新序号越过期望值，进入恢复。正常状态直接丢弃周期快照，合法或损坏快照都不影响连续增量。
3. **恢复时缓存两条流。** 快照按周期和帧序号收集，增量按全局序号保存，允许乱序到达。快照必须有 START、每个品种的 CLEAR、存量 ADD、END，帧序号完整且周期、水位一致。
4. **以水位 K 衔接。** 快照已经覆盖到 K，缓存中不大于 K 的增量丢弃，只补 K+1 起的连续更新。更晚的快照到来后采用新的周期和水位。
5. **完整后整批入队。** 快照只做一次发布端与接收端的状态校验，后续增量检查连续性和订单状态。批次按 CLEAR/ADD → 连续增量 → 结束标记排队；队列空间不足时整批重试。
6. **应用完成才放行策略。** 接收器负责正确拼接和入队；交易引擎只记录已应用到哪个代次，不重新拼接或计算全簿哈希。队列发布成功不代表另一个线程已经更新完订单簿。

例如，K=100 的快照恢复期间缓存了 101、102、104，因缺 103 不能提交。103 到来后按 101→102→103→104 补上；若采用新的 K=105 快照，就丢弃不大于 105 的缓存，从 106 继续。

恢复缓存有上限，总帧数超过 256×1024 时放弃当前候选，等待更晚周期。恢复也可能由增量报文截断、长度/解码/通道错误、下游队满，或本地簿拒绝状态转换触发。快照合成器周期广播，客户端没有向它发送恢复请求；单纯一段时间没数据当前不会触发恢复。

### 策略原来在哪里选择

`main` 接收算法参数，`TradeEngine` 创建 MAKER 或 TAKER，具体策略构造函数再把三个 `std::function` 绑定到自己。RANDOM 是主线程生成的模拟订单负载。它们的下单最终都通过统一风控与会话、行情准入检查。

## 解决问题

### 1. 在入口分清快照与增量

原来先解析任何通道的报文，错误时直接失效：

```cpp
if (socket->receive_fault_ || socket->next_rcv_valid_index_ % Common::Wire::MarketSize) {
  socket->next_rcv_valid_index_ = 0;
  stale();
  return;
}
// 解码后才进入 process()，正常状态忽略快照的判断位于那里。
```

现在正常状态在解析前就丢弃快照；增量还要求业务类型、合法品种和零快照元数据：

```cpp
const bool snapshot = socket == &snapshot_;
if (snapshot && !recovery_.recovering()) {
  socket->next_rcv_valid_index_ = 0;
  return;
}
// 解码后检查增量通道，控制帧不能交给普通订单簿。
if (!snapshot && (event.type_ < Type::CLEAR || event.type_ > Type::TRADE ||
                  event.ticker_id_ >= Common::ME_MAX_TICKERS ||
                  update.snapshot_cycle_ || update.watermark_ || update.state_hash_)) {
  stale();
  break;
}
```

上段 `Type` 为 `Exchange::MarketUpdateType` 的说明用别名。没有增加状态哈希或更改网络帧格式。

### 2. 统一策略接口，只在启动时选择

原写法在每个策略构造函数里重复绑定：

```cpp
trade_engine->algoOnTradeUpdate_ = [this](auto update, auto book) {
  onTradeUpdate(update, book);
};
```

现在 [Strategy](../trading/strategy/strategy.h) 规定订单簿、成交和回报三个虚函数，MAKER、TAKER 实现接口；[工厂](../trading/strategy/strategy_factory.cpp) 在启动时创建一个对象，交易引擎拥有 `std::unique_ptr<Strategy>`：

```cpp
strategy_ = createStrategy(algo_type, &logger_, &feature_engine_, &order_manager_, ticker_cfg);
// 热路径直接分派，既不读 JSON，也不反复判断策略名称。
if (marketReady())
  strategy_->onTradeUpdate(market_update, book);
```

RANDOM 仍在主线程产生测试负载，基类对象仅保留原来的事件日志。持仓、行情恢复、订单管理和最终风控没有交给具体策略，新增策略仍须经过统一发单口。扩展策略需要实现接口、登记工厂和允许的配置名称，不需要继续增加交易引擎的策略指针或回调绑定。

### 3. 两份 JSON 配置

- [exchange.json](../config/exchange.json)：本机组播网卡、快照和增量的地址/端口、TCP 监听地址/端口。
- [client.json](../config/client.json)：客户 ID、本机组播网卡、订阅地址/端口、TCP 远端、策略、默认与各品种参数、风险限额和模拟负载运行参数。

[读取与校验](../common/runtime_config.h) 使用固定的 Glaze v2.9.5，保留 C++20；官方头文件和许可随仓库保存，离线构建不下载依赖。字段拼错、缺必填项、非法策略、端口/地址/网卡错误或风险越界都在启动前报错。原通道含义保持：快照 `.1:20000`，增量 `.3:20001`。

原来主程序直接传字面量：

```cpp
OrderGateway(client, &requests, &responses, "127.0.0.1", "lo", 12345);
```

现在从客户端配置传值：

```cpp
OrderGateway(client, &requests, &responses, startup.tcp.address,
             startup.market_data.interface, startup.tcp.port);
```

交易所监听同样使用配置的明确地址；TCP 客户端的源地址仍由系统路由选择。两条 UDP 流使用不同端口，避免 Linux 默认组播分发使同端口流互相混入。

CMake 将示例复制到可执行文件旁的 `config/`，默认从那里读；`--config 文件` 指定其他路径，`--check-config` 只校验，不创建日志、审计或网络线程。原客户端命令行可覆盖 ID/算法/品种参数，三个测量用环境变量优先于 JSON 的 `runtime`，覆盖后统一再校验。

### 4. 特征与订单簿精简

[FeatureEngine](../trading/strategy/feature_engine.h) 只改空行、排版和中文注释，去除注释/空白后词法符号完全相同。保留关键说明：缺失特征用 NaN；微价格按对侧挂单量加权；先转 double 避免整数乘法溢出；主动买成交量比较卖一数量，主动卖比较买一数量。

[客户端簿](../trading/strategy/market_order_book.h)和[备用对照簿](../exchange/matcher/unordered_map_me_order_book.h)复用已验证的统一价档插入：空簿建自环；新最优或新最差直接插入；中间档沿链找位置；最后统一连前后指针。同价订单尾插 FIFO、完整价格键和对象池保持原样。

原来同一判断在循环里重复，并维护前后两套链接分支：

```cpp
bool add_after = ((new_orders_at_price->side_ == Side::SELL &&
                   new_orders_at_price->price_ > target->price_) ||
                  (new_orders_at_price->side_ == Side::BUY &&
                   new_orders_at_price->price_ < target->price_));
if (add_after) {
  target = target->next_entry_;
  add_after = ((new_orders_at_price->side_ == Side::SELL &&
                new_orders_at_price->price_ > target->price_) ||
               (new_orders_at_price->side_ == Side::BUY &&
                new_orders_at_price->price_ < target->price_));
}
// 后续 while 再重复比较，之后分别维护插到 target 前/后的链接。
```

现在只确定插入点，再统一插到它前面：

```cpp
auto* target = best;
if (!new_best && better(level->price_, best->prev_entry_->price_)) {
  do { target = target->next_entry_; }
  while (target != best && better(target->price_, level->price_));
}
level->prev_entry_ = target->prev_entry_;
level->next_entry_ = target;
target->prev_entry_->next_entry_ = level;
target->prev_entry_ = level;
if (new_best) best = level;
```

`addOrder()` 的已有价档分支还去掉重复判空，直接取该价档的首单。原来的分支并非业务错误，问题是重复判断和最差档不必要的遍历。`#pragma region` 原文保留。

## 重新测试

### 功能结果

- 配置解析与校验：98 项检查通过。
- 主程序入口：16 个默认配置、命令行覆盖、空/重复/缺失路径、非法数字及环境变量案例通过；检查模式和配置错误均未创建运行资源。
- 非默认端点订单闭环：TCP `127.0.0.2:29015`；快照 `233.252.15.11:29011`；增量 `233.252.15.13:29013`。另一回环地址 `127.0.0.1` 无法连接该端口，证明监听没有忽略配置地址。
- 实际运行产生 12 个配置客户 ID=7 的新单；28 条 RECEIVED 与 APPLY 逐条一致，接到 24 条增量和 414 帧快照；本地完成恢复，停机队列清空、会话健康、无未决风险预占。
- MAKER/TAKER 旧新行为一致：报价、阈值、主动买卖方向、接受后不重发、撤回后换单、行情未恢复或再次失效时禁止策略、超风险额度拒绝请求；RANDOM 不因行情回调产生新订单。
- 两种簿各通过 12000 固定种子模型事件，检查成交回报、FIFO、剩余数量、本地状态及每一步 BBO 数量；覆盖两侧 128 档升序/降序/随机插入和反向撤档。
- 通道错误旧版两项失败，新版全部通过；完整快照、恢复应用边界与风控定向测试通过。
- 最终 Release 的 10 项定向测试全部通过，覆盖配置、真实策略、通道边界、两种簿、协议、风控、特征与恢复。ASan/UBSan 的 6 项定向测试全部通过，未报告内存或未定义行为错误。

### 性能结果

固定 CPU 3，旧新版本交替 7 轮，不在测量窗口编译或运行其他验收。组件的 P50/P99 是批内平均单次耗时的分位数，再取 7 轮中位数；构造和预热不计入。配对变化先算每一轮的“新版/旧版−1”，再取中位数，负数表示变快。环境随时间波动时，两组中位数的比值可能与配对变化不同。

| 对照 | 旧 P50 | 新 P50 | 旧 P99 → 新 P99 | 配对 P50 变化 | 新版更快轮数 |
|---|---:|---:|---:|---:|---:|
| 单独策略分派，ns/调用 | 4.528 | 7.439 | 9.807 → 30.685 | +66.25% | 1/7 |
| MAKER 实际回调，μs/调用 | 23.681 | 23.538 | 57.240 → 69.068 | +0.45% | 3/7 |
| TAKER 实际回调，μs/调用 | 25.341 | 26.218 | 59.560 → 67.498 | +3.46% | 3/7 |
| 合法增量接收，ns/帧 | 47.660 | 52.321 | 107.286 → 98.003 | +9.02% | 1/7 |
| 客户端新最优价档 ADD，ns/更新 | 9047 | 8276 | 24616 → 21551 | −9.14% | 7/7 |
| 客户端新最差价档 ADD，ns/更新 | 9685 | 8814 | 31018 → 25761 | −10.20% | 6/7 |
| 客户端随机价档 ADD，ns/更新 | 9164 | 9140 | 21802 → 28058 | −1.07% | 5/7 |
| 备用簿新最差价档 ADD，ns/更新 | 591 | 474 | 3065 → 2551 | −19.77% | 7/7 |
| 备用簿随机价档 ADD，ns/更新 | 551 | 534 | 3213 → 3335 | −2.91% | 7/7 |

策略每批 64 次、每轮 12800 次调用，保留实际日志并检查丢弃数。分派探针每批 30 万次，使用相同计算体、独立工厂翻译单元并关闭 LTO。新版分派各轮 P50 在约 4.2～16.4 ns 间波动，不能把初次短测的“只增加 0.43 ns”当成稳定结论。真实策略的变化包含日志、特征与状态处理，不能全部归于分派。

增量接收每轮处理 1000 万帧，100 批×10 万帧，核对预热与测量合计 10000100 次队列交付；新增通道检查在该场景观察到约 5 ns 的中位成本。客户端簿每批 192 次 ADD、300 个测量批次，含输入校验、BBO 和日志格式化入队。基准程序将日志写到 `/dev/null` 并使用较大的日志队列，业务日志策略未变。98 次最终组件运行全部通过计数与状态检查，日志丢弃为 0。随机价档的中位收益很小，P99 上升，不能称全面改善。

备用簿每轮 100 批预热、5000 个测量批次，固定相同插入和撤单，校验状态后才采用结果。初期短窗口结果明显漂移；误用模式参数产生的非目标场景也已保留并排除。加强复测两场景均有 7/7 对中位收益，但随机插入 P99 仍略高。

### 交易所订单往返

旧新各在 50/s、500/s 下交替测 7 轮，共 28 轮。每轮 128 条预热、256 条正式请求，固定种子、4 个客户、8 个品种，交替 NEW/CANCEL，保留普通日志和同步审计。旧新输入逐字节相同。下面的 RTT 从外部发单器发送完整 TCP 请求到收到已校验回报，覆盖交易所 TCP、定序、撮合和回报路径；本次负载没有跨单成交，也不包含客户端策略、OrderGateway 和行情消费器。

| 发单速率 | 旧 RTT P50，ms | 新 RTT P50，ms | 旧 RTT P99 → 新 RTT P99，ms | 配对 P50 变化 | 新版更快轮数 |
|---|---:|---:|---:|---:|---:|
| 50/s | 8.659 | 8.639 | 13.899 → 14.267 | +5.86% | 3/7 |
| 500/s | 1155.385 | 1074.561 | 1829.849 → 1515.636 | +1.82% | 3/7 |

各组中位数略低，逐轮配对仍只有 3/7 对更快，不能据此宣布整条订单链稳定提速。默认撮合簿的热路径本次未修改，策略和客户端簿也没有进入这条外部发单器测试链。

500/s 的 256 条请求只用约 0.512 秒发完，窗口结束时仍有旧版中位 251 条、新版 252 条未完成；请求队列高水位分别为 208～235、202～234，最后才全部排空。50/s 则在窗口内完成全部请求，请求高水位只有 1～2。秒级 RTT 与积压相符，不能解释成队列一次入出操作需要一秒。

28 轮业务对账全部通过：每轮 384 条 RECEIVED/APPLY/RESPONSE 一致，7168 条正式请求及 3584 条预热请求均完成，停机队列为空、无未应用事件，诊断日志丢弃为 0。内存压力基本为 0，CPU 与 I/O 等待有波动。程序仍保留三次同步审计 fsync；历史分段测量见 [第 6 篇](06_latency_and_environment.md)。本轮没有新增分段计时，不能给出这轮精确的 fsync 占比，也没有证据认定主机性能永久下降。

### 如何复测

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRADE_PERF_TRACE_EVERY=0
cmake --build build/release -j 2 --target exchange_main trading_main runtime_config_test \
  strategy_behavior_test strategy_dispatch_benchmark recovery_channel_test \
  client_book_benchmark book_model_test control_probe recovery_test protocol_test
ctest --test-dir build/release -R '^(runtime_config_contract|strategy_behavior|recovery_channel|reliability_book|reliability_unordered_book|reliability_features|reliability_final_risk_gate|reliability_recovery|reliability_recovery_apply|reliability_protocol)$' --output-on-failure

# 使用一个不存在的证据目录，保留配置、报文和订单审计。
python3 tests/09_runtime_config/startup_integration_test.py \
  build/release config /tmp/trade-config-check-01

# 独立测分派；旧/新对照仍需保存各自源码与二进制。
taskset -c 3 build/release/strategy_dispatch_benchmark function
taskset -c 3 build/release/strategy_dispatch_benchmark virtual
taskset -c 3 build/release/strategy_behavior_test maker
taskset -c 3 build/release/strategy_behavior_test taker
taskset -c 3 build/release/recovery_channel_test benchmark
taskset -c 3 build/release/client_book_benchmark worst
```

正式行情接收基准已同步为 1000 万帧长窗口，重新构建并通过通道测试，交付计数 10000100 正确。原始基线、旧/新二进制、组件样本、逐笔 RTT、压力数据及所有慢轮和中间失败保留于验收证据目录；本次没有执行全量回归。非默认端点闭环验证配置与状态，组件对照验证局部成本，外部订单往返验证指定 NEW/CANCEL 负载，三种结果分别解读。

<details>
<summary>2026-10-12 价格档与恢复简化：原代码对照、验收和历史数据</summary>

以下保留上一轮实验，使用其原基线和测量条件；本次 2026-10-13 结果见上文。

## 上一轮：订单簿插入与行情恢复简化

验证日期：2026-10-12。工作区基线：`f52d1f0` 加已有网络注释整理；以下改动尚未提交。测试环境为 Ubuntu 26.04 虚拟机、GCC 15.2、C++20、Release，关闭诊断计时，测量线程绑定逻辑 CPU 3。

## 1. 测试时发现的问题

改造前，现有订单簿模型和恢复故障矩阵均通过，没有复现新的撮合错误。问题主要是重复代码和重复计算：

- `addOrdersAtPrice()` 反复判断买卖方向，并维护两套接入双向环的代码。插入最差档时仍从最优价扫描。
- 恢复器把重复帧转为字符串再比较；只有一份快照时，也重新计算“最终批次哈希”。
- 交易引擎应用完批次后，再收集全部本地订单、排序和计算哈希。校验职责和计算在两条线程间重复。

先保存原工作区和基准，再分别改价格档与恢复路径。第一版价格档简化通过模型测试，但随机插入的中位成本为 748 → 793 ns，约慢 6%；没有将这一轮写成提速。随后增加最差档直接追加，再重新测量。

## 2. 寻找问题

### 价格档为什么能少写一套插入代码

买侧按价格从高到低、卖侧从低到高排列。它们都是双向环：`best->prev_entry_` 就是最差档。

旧代码分别处理“插在 target 后”和“插在 target 前”，其实都只是连接新节点的前后指针。先确定一个插入位置，再统一插在它前面，就能覆盖新最优、中间和新最差三种情况。

### 为什么仍需一个恢复结束标记

行情接收器能证明输入完整，并把事件一次发布到队列；但此时交易引擎可能只消费了半批。若入队时立即开放策略，策略可能在旧簿或半恢复的簿上运行。

结束标记只标出 FIFO 队列中的应用边界。数据正确性由接收器校验；交易引擎走到标记时，只记录这批数据已应用到哪个代次，不再复查全簿哈希。遇到缺口或应用失败就推进当前代次，旧标记无法重新开放策略。

## 3. 解决问题：原代码与改后代码

### 3.1 价格档插入

文件：[`me_order_book.h`](../exchange/matcher/me_order_book.h)。旧实现重复的方向判断：

```cpp
bool add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                  (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
```

上述表达式在查找过程中出现三次；之后还分别连接“前插”和“后插”的指针。改后函数如下：

```cpp
  auto addOrdersAtPrice(MEOrdersAtPrice* level) noexcept {
    price_orders_at_price_[{level->side_, level->price_}] = level;
    const bool buy = level->side_ == Side::BUY;
    auto*& best = buy ? bids_by_price_ : asks_by_price_;
    if (UNLIKELY(!best)) {
      best = level;
      level->prev_entry_ = level->next_entry_ = level;
      return;
    }

    // 买价降序、卖价升序；新档更优时，直接插在 best 前。
    const auto better = [buy](Price left, Price right) {
      return buy ? left > right : left < right;
    };
    const bool new_best = better(level->price_, best->price_);
    auto* target = best;
    // 比尾档更差时直接追加，无需绕价格环一周；只有中间档需要查找。
    if (!new_best && better(level->price_, best->prev_entry_->price_)) {
      do {
        target = target->next_entry_;
      } while (target != best && better(target->price_, level->price_));
    }

    // 统一插在 target 前；绕回 best 时，等价于追加到最差档之后。
    level->prev_entry_ = target->prev_entry_;
    level->next_entry_ = target;
    target->prev_entry_->next_entry_ = level;
    target->prev_entry_ = level;
    if (new_best)
      best = level;
  }
```

空侧建立自环；新最优价直接更新 `best`；新最差价利用尾指针直接追加；中间档保持一次线性查找。最优、最差插入为 O(1)，中间插入仍为 O(价格档数)。同价订单的 FIFO 追加规则不变，已有价格档不会进入这个函数。

同时删除 `addOrder()` 的冗余判空：已经进入非空分支，直接取 `orders_at_price->first_me_order_`。

### 3.2 重复帧比较与恢复构造

文件：[`market_recovery.h`](../trading/market_data/market_recovery.h)。旧写法：

```cpp
found->second.me_market_update_.toString() != update.me_market_update_.toString()
```

改为直接比较业务字段：

```cpp
std::tie(a.type_, a.order_id_, a.ticker_id_, a.side_, a.price_, a.qty_, a.priority_) ==
std::tie(b.type_, b.order_id_, b.ticker_id_, b.side_, b.price_, b.qty_, b.priority_)
```

用 `try_emplace()` 合并查找和插入，重复帧不分配节点。快照的水位与声明哈希仍参与冲突判断。快照序号由有序 map 去重保存；起点为 0 时，尾序号不等于帧数减一，就先等待缺帧，不重建临时订单簿。

保留一次“实际快照状态是否等于发布端声明”的校验；删除 `batch.hash = Common::bookHash(flatten(state))`。续接增量仍逐条检查序号和订单状态，`TRADE` 不扣簿，`MODIFY` 覆盖绝对剩余量，`CANCEL` 删除挂单。批次事件和哈希输入预留容量，旧增量用 `upper_bound()` 定位边界。

### 3.3 交易引擎不再重复算哈希

文件：[`trade_engine.cpp`](../trading/strategy/trade_engine.cpp)、[`trade_engine.h`](../trading/strategy/trade_engine.h)、[`market_data_consumer.cpp`](../trading/market_data/market_data_consumer.cpp)。旧路径的主要开销：

```cpp
for (const auto* book : ticker_order_book_) {
  const auto live = book->liveOrders();
  orders.insert(orders.end(), live.begin(), live.end());
}
const auto hash = Common::bookHash(orders);
const bool verified = hash == market_update->state_hash_ &&
                      market_update->priority_ == market_generation_.load(std::memory_order_acquire);
market_trusted_.store(verified, std::memory_order_release);
```

改后，走到结束标记只登记已应用代次：

```cpp
const auto generation = market_update->priority_;
if (generation == market_generation_.load(std::memory_order_acquire)) {
  applied_generation_.store(generation, std::memory_order_release);
}
```

是否可使用行情：

```cpp
const auto applied = applied_generation_.load(std::memory_order_acquire);
return applied == market_generation_.load(std::memory_order_acquire);
```

当前代次从 1 开始，已应用代次从 0 开始。发现缺口只需推进当前代次，两个数立即不同；旧标记即使迟到，也不能变成当前代次。过期标记直接忽略，不额外推进代次。原 `trusted` 布尔值及其跨线程写入被删除。

内部行情对象不再保存仅供结束标记使用的 `state_hash_`。网络帧里的快照哈希、版本与编码字段保留，结束标记只在进程内队列使用。

### 3.4 网络不能携带内部恢复结束标记

文件：[`market_protocol.h`](../common/market_protocol.h)。原解码器读完字段后直接 `return true`，没有区分公开行情与内部控制标记。新增反例向网络解码器输入 `RECOVERY_COMMIT`：旧代码接受，测试退出 1；改后拒绝，测试通过。

恢复标记必须由接收器完成校验后生成。改后只接收公开类型：

```cpp
return r.type_ >= Exchange::MarketUpdateType::CLEAR &&
       r.type_ <= Exchange::MarketUpdateType::SNAPSHOT_END;
```

拒绝 `INVALID`、`RECOVERY_COMMIT` 和未知类型。70 字节帧长、版本、大小端及业务字段不变。已有恢复用例增加全部 7 种公开类型与 3 种非法类型的对照，避免去掉重复哈希后让网络标记绕过恢复流程。

### 3.5 注释整理

原项目 C++ 源码、示例、基准及现有测试中的英文说明已译成中文或合并，保留类型名、协议名及日志文字。纠正品种 ID 范围、数组与 map 的描述，统一长注释的位置和短注释间距。所有 `#pragma region` 保留。

纯注释改动逐文件比较去除注释和空白后的代码符号，34 个文件一致；业务改动另用下述模型和故障用例验证。

## 4. 重新测试：正确性与性能

### 正确性

Release 下通过订单簿、行情恢复、恢复应用边界、最终风险准入、网络协议 5 项针对性测试。价格档增加 256 档边界场景，并运行 12,000 条固定随机事件，与价格/FIFO 参考模型对照回报、量、BBO 和双方订单状态。

恢复测试覆盖乱序、缺帧补齐、全部业务字段的重复冲突、元数据冲突、坏快照哈希、缓存上限、增量缺口、TRADE/MODIFY/CANCEL 语义和整批准入。应用边界测试确认：半批不调用策略、旧标记不开放策略、应用失败后旧标记无效、新批次完成后才继续策略。

地址与未定义行为检查（ASan/UBSan）通过订单簿、恢复和恢复应用边界 3 项测试。`git diff --check` 通过。

### 价格档性能

为单独看插入算法，最终对照使用两个独立源码副本：除该函数与已知非空分支的冗余判空外，其余业务源码和消息布局相同。每轮增加 192 笔不互相成交的订单（买卖各 96 档），预热 30 轮、记录 300 轮，旧/新交替测量 7 次。

下表单位 ns。p50/p99 是“每轮新增总耗时除以 192”的分位数，再取 7 次测量的中位值；不是单笔网络请求的延迟分位数。

| 场景 | 旧 p50 | 新 p50 | 耗时下降 | 旧 p99 → 新 p99 |
|---|---:|---:|---:|---:|
| 持续插入更优价 | 341.000 | 349.000 | -2.3% | 4021.000 → 3946.000 |
| 持续插入更差价 | 655.000 | 405.000 | +38.2% | 4594.000 → 4065.000 |
| 固定随机价格 | 630.000 | 588.000 | +6.7% | 4519.000 → 6035.000 |

持续更优价的 p50 本轮略慢 2.3%；随机插入的 p99 从 4,519 增至 6,035 ns。本次没有证明所有插入路径或尾延迟都会下降，收益主要来自最差档直接追加。

### 行情恢复性能

每个场景预热 100 次，记录 1,000 次；新旧交替 7 轮。表中为各轮 p50/p99 的中位值，单位微秒。结束标记测量前已建好订单簿，计时只包含消费标记，不包含整个批次重放。

| 场景 | 旧 p50 | 新 p50 | 耗时下降 | 旧 p99 → 新 p99 |
|---|---:|---:|---:|---:|
| 64 单完整恢复 | 39.974 | 27.342 | +31.6% | 123.067 → 98.813 |
| 1,024 单完整恢复 | 814.624 | 470.048 | +42.3% | 4578.888 → 1335.656 |
| 64 单及 64 次重复快照帧 | 151.205 | 28.444 | +81.2% | 447.683 → 98.341 |
| 64 单续接 MODIFY、TRADE | 41.371 | 27.700 | +33.0% | 135.677 → 92.533 |
| 缺一帧、尝试 8 次后补齐 | 45.421 | 27.073 | +40.4% | 190.514 → 105.218 |
| 处理结束标记：64 单 | 17.503 | 6.132 | +65.0% | 95.134 → 64.075 |
| 处理结束标记：1,024 单 | 195.009 | 6.148 | +96.8% | 657.176 → 48.956 |

正常准入另用原有 `admission_benchmark` 交替测量 7 轮，每轮 10,000 次；原版本平均单次成本的轮间中位值为 19.528 微秒，新版本为 19.219 微秒。差异约 1.6%，有快有慢，不作为稳定提速结论。

网络类型检查另测正常帧解码：预热 10,000 次、测量 1,000,000 次，交替 7 轮。平均单次成本的轮间中位值为 23.992 → 24.109 ns，增加约 0.117 ns（0.5%）。该差异很小，不作稳定变慢结论；保留类型边界检查。

虚拟机仍有明显慢样本，全部保留，没有删除慢轮。以上数字说明组件和恢复结束处理的成本下降，不能证明整条报单链路、缺包后等待快照或整个恢复批次重放同幅度提速；CLEAR 扫描和网络等待仍有成本。

### 复测入口

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTRADE_PERF_TRACE_EVERY=0
cmake --build build --target exchange_main trading_main book_model_test recovery_test control_probe protocol_test book_recovery_benchmark recovery_commit_benchmark wire_decode_benchmark -j2
ctest --test-dir build -R '^(reliability_book|reliability_recovery|reliability_recovery_apply|reliability_final_risk_gate|reliability_protocol)$' --output-on-failure
taskset -c 3 build/book_recovery_benchmark book_random
taskset -c 3 build/book_recovery_benchmark recovery_snapshot_64
taskset -c 3 build/book_recovery_benchmark recovery_duplicates
taskset -c 3 build/recovery_commit_benchmark 1024
taskset -c 3 build/wire_decode_benchmark
```

正式用例在 `tests/01_reliability/`，基准在 `tests/08_book_recovery/`。改造前源码、逐轮数据、编译及测试日志归档于虚拟机 `trade-measurements/10_book_recovery_cleanup/`，初版慢结果也保留。报告和业务代码目前都在工作区。

</details>


</details>
