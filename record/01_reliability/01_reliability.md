# 01 可靠性改造：先修正业务结果，再看耗时

修复范围：`c88c13b → 9572484`。文档核对：2026-10-04。

主要问题已经修复：队列覆盖、TCP 丢尾部、内存池容量、价格碰撞、订单状态、风险预占和不完整行情恢复。补充了线程排空及关键交易审计。逐项旧代码和新代码见[源码对照](01_source_changes.md)。

## 测试发现了什么

| 最小输入或场景 | 旧代码的问题 |
|---|---|
| 容量 2 的队列写第 3 条 | 覆盖未读消息 |
| 暂停接收，小 TCP 发送缓冲 | 待发长度归零，尾部没有送到 |
| 容量 2 的内存池 | 第 2 次合法分配就失败 |
| 33 条相同时间戳请求 | 原到达顺序改变 |
| 同侧价格 100、356 | `%256` 后碰撞，档位混淆 |
| 撤单被拒绝 | 卡在 PENDING_CANCEL |
| 两笔买单各 6，持仓上限 10 | 不计在途单，可能超过限制 |
| 快照缺少品种 CLEAR | 仍被当成恢复成功 |
| 多线程记录业务时间 | TSan 报共享缓冲竞争 |

原始失败分别保留在 [baseline_results.json](baseline_results.json)、[book_baseline_failures.json](book_baseline_failures.json)、[control_baseline_failures.json](control_baseline_failures.json)、[recovery_baseline_failure.json](recovery_baseline_failure.json)、[concurrent_baseline_failure.json](concurrent_baseline_failure.json)。

## 问题在哪里，怎样解决

**队列需要完整的交付结果。** 改为 `try_push/try_pop`，满时返回 false，原内容不变。FIFO 保留未发布请求；网关保留完整帧；关键回报不能无声丢弃；行情不足则标为不可信并恢复。Logger 的多写入者先串行化，整条日志接受或丢弃并计数。

这里的 acquire/release 保护的是槽位所有权：生产者先写数据，再 release 发布 tail；消费者 acquire 看到 tail 后才能读。消费者复制完再 release 发布 head，生产者 acquire 看到后才能复用槽。它仍是 SPSC，不能直接当成多生产者队列。

**TCP 需要保留发送进度。** `send` 只表示内核接受了多少字节；按实际字节数推进，EAGAIN 留待下轮。整帧进入缓冲后才出队、推进序号；EOF 和连接错误进入可观察状态。断线不等于订单未成交，不能自动重发。

**订单簿需要完整身份。** 价格档用 `(side,price)`，不能用取模结果代表真实价格。客户订单改为稀疏映射；操作前检查字段、重复 ID 和容量。内存池使用空闲槽表，容量 N 可完整用 N 个槽，并正确构造、析构对象。同时间戳使用稳定排序。

**风险预占必须覆盖待成交单。** 最终发送口统一检查所有策略，包括 RANDOM。分别计算“全部买单成交”和“全部卖单成交”的最坏持仓；发送失败回滚额度。撤单请求发出时原单还可能成交，不能立即释放额度。重复成交回报先去重，再更新持仓。

**恢复成功需要证明簿一致。** 快照必须同周期、同水位，包含全部 8 个品种的 CLEAR，并通过状态哈希。后续增量连续衔接；完整恢复批次一次发布，TradeEngine 再核验本地簿，通过后才允许新单。

**停机与审计独立处理。** 各模块持有线程，停止后 join；先停新输入，再按依赖排空。关键审计每条 write 后 fsync，已有文件用 O_EXCL 防止覆盖。APPLY 只表示进入处理，并非完整事务提交；[reconcile.py](reconcile.py) 只读对账，不自动重放。

## 修复后怎样验证

Release 22 项 CTest 通过；历史 Debug、ASan/UBSan、TSan 和多客户端结果分别保留在验证文件中。2026-10-04 对当前全量日志构建重新运行，22/22 仍通过，见 [04 复查结果](../04_source_repairs/evidence/ctest_release.txt)。不同验证批次不混成同一次测试。

两套撮合簿分别与价格/FIFO 参考模型比较 12,000 个固定种子事件；同时核对回报、活动订单、BBO 和客户端簿。真实进程验证两客户端、晚加入恢复、非法协议、只读查询、正常退出与已有审计文件重启拒绝。详情见 [integration_result.json](integration_result.json)、[book_model_test.cpp](book_model_test.cpp)、[recovery_test.cpp](recovery_test.cpp)。

## 性能变化

每项都先保存基准，改一项、验证、再用相同有效输入测五轮。以下为历史五轮中位数；旧版部分故障场景不正确，因此它们是修复成本对照，不是整个交易系统的提速证明。

| 项目和单位 | 修复前 | 修复后 | 说明 |
|---|---:|---:|---|
| 队列 push+pop，ns | 237.598 | 16.4689 | 满队列不再覆盖 |
| TCP 512 字节收发，ns | 20,620.2 | 6,785.19 | 相同完整数据流 |
| 池分配+归还，ns | 323.932 | 90.7674 | 最后一槽可用，析构正确 |
| 64 请求排序，ns | 23,736.4 | 22,911.5 | 幅度小，主要修顺序 |
| NEW/CANCEL，ns | 75,277 | 75,776.1 | 增加 0.66%，基本持平 |
| 风控循环，ns | 95,140.9 | 95,915.3 | 增加 0.81% |
| 请求编码+解码，ns | 32.9082 | 89.961 | 帧与功能扩大，增加成本 |
| 恢复组件一轮，ns | 1,004,680 | 58,870.5 | 输出校验一致，含日志路径变化 |
| 品种特征更新，ns | 14,247.9 | 3,270.96 | 含日志变化，并非纯计算收益 |
| 创建线程+join，ns | 1,000,561,578 | 491,661 | 主要去掉固定一秒等待 |
| 时间格式化，ns | 5,701.05 | 1,734.33 | 并发缓冲竞争也已修正 |

全部原值、比例和前后 JSON 链接见 [performance_summary.json](performance_summary.json)。审计/停机阶段，ACK P99 从 4.608310 增到 5.033436 ms；最终独立批次为 1.904154 ms。两批不能混作单项 fsync 收益，见 [perf_e2e_before.json](perf_e2e_before.json)、[perf_e2e_after.json](perf_e2e_after.json)、[perf_e2e_final.json](perf_e2e_final.json)。

单本簿 sizeof 从约 2 GiB 降到 29,040 字节，主要减少固定索引和虚拟地址占用。实测 RSS 基本不变，不能说释放了 18 GiB 物理内存。

## 仍有边界

已覆盖的正常路径和故障注入通过；不是任意故障、任意负载都已验收。没有撮合进程的自动崩溃重放，不自动批准未知账户恢复交易。恢复组件耗时不等于真实网络恢复总时间。

复测入口：

```bash
cmake -S . -B build/reliability-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/reliability-release -j 4
ctest --test-dir build/reliability-release --output-on-failure
```

<a id="source-comparison"></a>

26 项具体修改见 [01_source_changes.md](01_source_changes.md)。
