# 04 源码修复核对：十类问题与补充验收

验证日期：2026-10-04。源码起点：`afcadfb`，另有已登记的性能日志与快照接口候选。旧错误基线：`ff20fd7`。

十类主要问题的实现已在可靠性和测量改造中修复。本轮核对代码、重跑回归，并补充槽位交错、持仓成本及重复 CLEAR 检查，**没有新增生产源码改动，也没有新的性能提升数字**。

## 测试发现了什么

已有回归覆盖主要业务，但两个结论需要更直接的证据：

1. 发布模块释放队列槽后，生产者若立即覆盖该槽，转发给快照的消息是否仍是原消息？
2. 持仓从多头减仓再转空头，数量、已实现盈亏和成本是否正确？

此外，再检查首次 ADD、撤最后一单、重复 CLEAR 和同价重建。

## 哪些修复已经完成

| 项目 | 已完成的实现 | 对应验收 |
|---|---|---|
| R1 队列覆盖 | 有界 try_push/try_pop，失败不改旧消息；调用方处理满队列 | 容量 1/2/3、三轮百万序号 |
| R2 TCP 丢尾部 | 按实际字节推进，保留 EAGAIN 余量；完整帧、EOF/错误状态 | 背压流逐字节一致、拆合包、拒绝连接 |
| R3 同时间戳重排 | stable_sort 保持同戳收集顺序 | 33 条同戳请求保持顺序 |
| R4 内存池边界 | 空闲槽表、正确构造析构；容量 N 全部可用 | 最后一槽、耗尽、回收及析构计数 |
| R5 未初始化索引/持仓 | 指针表和成本数组初始化，客户订单表稀疏化 | 参考模型；本轮堆/栈及八品种初始化 |
| R6 价格碰撞 | 两套簿均使用完整方向和价格 | 每套 12,000 事件与参考模型对照 |
| R7 本地簿状态 | 先校验变更，CLEAR 清理索引并重算 BBO | 逐事件簿对照；本轮重复 CLEAR/同价重建 |
| R8 槽释放后再读取 | 释放前复制消息，快照使用本地副本 | 本轮确定性交错及生产函数检查 |
| R9 线程生命周期 | 按值持有任务、原子停止、对象持有线程并 join | 生命周期、并发发送及正常排空 |
| R10 输入/数值/测量 | 字段/身份校验、isfinite、逐品种特征、宽数量、正确测量分母 | 协议、风控、特征及测量工具回归 |

具体历史代码与实现提交见 [01 源码对照](../01_reliability/01_source_changes.md)、[02 源码对照](../02_reproducible_measurement/02_source_changes.md)。

## 两处关键技术细节

### 队列槽只能在持有期间读取

旧发布流程的顺序是：取得槽指针 → 释放槽 → 用旧指针构造快照消息。释放后，生产者已经有权覆盖该槽；指针地址没有变，不代表消息仍然有效。

当前 `MarketDataPublisher::run()` 先复制，再释放：

```cpp
const auto update_copy=*market_update;
outgoing_md_updates_->pop();
```

后续快照使用副本：

```cpp
const MDPMarketUpdate snapshot_update{next_inc_seq_num_,update_copy};
```

修改位于 `5f70930`。本轮用容量 1 队列和原子屏障强制这一顺序：消费者释放 → 生产者写 222 → 消费者转发。旧流程期望转发 111，实际为 222，测试失败；先复制的流程仍转发 111。

交错测试验证的是队列和转发适配器的槽位契约，并未给生产线程植入暂停点。生产函数另检查复制位于 pop 前、释放后不再解引用旧指针；完整行情链路的历史验收见 01。

### 持仓成本必须跟着剩余方向走

`PositionInfo::open_vwap_` 保存的是未平仓总成本，除以剩余数量才是均价。它从零初始化；减仓按原均价减少成本，反向开仓时旧方向成本清零，新方向按本次成交价建立成本。

本轮直接调用实际 `PositionInfo::addFill()`，得到：

| 成交 | 持仓 | 累计已实现盈亏 |
|---|---:|---:|
| 买 10 @ 100 | +10 | 0 |
| 卖 4 @ 110 | +6 | 40 |
| 再卖 8 @ 90 | −2 | −20 |

最后空头总成本为 180，多头成本为 0。堆、栈及八个品种的初始持仓/成本也均为零。

## 重新测试的结果

| 检查 | 结果 |
|---|---|
| 当前全量日志 Release 构建的 CTest | 22/22，49.36 秒 |
| 旧队列/旧转发流程的交错反例 | 退出 1，明确复现 111 被转发成 222 |
| 当前实现的三个补充检查 | Release、ASan/UBSan、TSan 均退出 0 |
| 本地簿序列 | 首次 ADD、同价合量、撤最后一单、CLEAR 两次、同价重建均通过 |
| 生产源码 | 与核对起点相比未新增修改 |

原输出见 [ctest_release.txt](evidence/ctest_release.txt)、[baseline_checks.txt](evidence/baseline_checks.txt)、[release_checks.txt](evidence/release_checks.txt)、[asan_checks.txt](evidence/asan_checks.txt)、[tsan_checks.txt](evidence/tsan_checks.txt)。编译命令、源文件/二进制/库摘要见 [focused_checks.json](evidence/focused_checks.json)，生产函数契约见 [publisher_contract.json](evidence/publisher_contract.json)。

ASan/UBSan 检查内存越界和未定义行为，TSan 检查线程数据竞争。本轮只覆盖补充检查及其链接的已有插桩库；并非整个项目所有路径都做了这轮检查。测试版本和文件摘要见 [review_manifest.json](evidence/review_manifest.json)。补充工具 `review_checks.cpp` 保留在本目录工作区，尚未提交；原始编译输入也保存在 VM 的 `/home/zjh/trade-measurements/04/`。

## 性能与未完成范围

没有新的生产实现改造，因此不重复制造一组“改前/改后提速”。可靠性阶段的逐项历史基准见 [01 性能汇总](../01_reliability/performance_summary.json)，性能候选的五轮对照见 [03](../03_performance_tuning/03_performance_tuning.md)。以后新增单项改动，仍按基准 → 修改 → 正确性 → 同条件复测执行。

当前排序只保证批次内同时间戳稳定，尚未证明跨批次全局公平。未做 MemorySanitizer，也未穷尽所有部分构造失败及满队列停机交错。已有审计可对账，但没有自动崩溃重放。上述范围不写成已完成。
