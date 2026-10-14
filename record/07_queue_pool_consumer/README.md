# 07 的定向测试与性能基准

结论和原／新代码见 [07 报告](../07_book_recovery.md)。这里把本轮测试与报告放在一起；性能基准不加入自动 CTest。

| 文件 | 用途 |
|---|---|
| capacity_contract.cpp | 编译期检查常量标签，拒绝 0、3 和超过两圈范围的容量 |
| queue_semantics.cpp | peek/pop 混用、空批次、满队失败、高水位及 Logger 多线程串行提交 |
| consumer_regression_test.cpp | 回报字段、快照状态、失败诊断、定序背压与日志精确字节 |
| queue_benchmark.cpp | 单条、批量、跨核、满队失败的整窗平均耗时 |
| consumer_benchmark.cpp | 快照缓存、回报编码、单条及批量定序的批均值分位数 |
| trade_guard_probe.h / trade_guard_regression.cpp / trade_guard_benchmark.cpp | 真实交易引擎消费、本地簿、特征、回调和恢复代次 |
| pool_benchmark.cpp | 真实节点借出／归还，包含构造与字段验证 |
| baseline/common/lf_queue.h | 本轮开始时的旧队列，只供对照，不是生产头文件 |
| rejected/common/mem_pool.h | 标记分离候选，实测变慢，未采用 |
| *_measurement.json | 全部配对统计；慢轮和尾延迟保留 |

基线来自当时完整工作区，已包含前一轮策略与配置改造，不能用只检出旧 HEAD 的源码冒充相同基线。队列旧头与未采用池头在此保留，完整实验环境与原始运行证据另有归档。

按根 README 构建 Release 后，定向检查：

```bash
ctest --test-dir build/release \
  -R '^(queue_pool_(queue|pool|fifo)|queue_capacity_contract|queue_semantics|consumer_hotpath_contract|trade_engine_consumer_contract)$' \
  --output-on-failure
```

队列基准示例：CPU 3 为生产核、CPU 2 为消费核；其他机器按实际可用 CPU 调整。正式实验固定数量，旧、新版交替运行 7 对，保留每一轮。

```bash
build/release/queue_hotpath_benchmark pair 40294880 3 2
build/release/queue_hotpath_benchmark batch 51736144 3 2
build/release/queue_hotpath_benchmark cross 4285792 3 2
build/release/queue_hotpath_benchmark full 58133952 3 2

# 旧队列头优先，其余依赖使用同一份当前源码。
g++ -std=c++20 -O3 -DTRADE_PERF_TRACE_EVERY=0 \
  -Irecord/07_queue_pool_consumer/baseline -Icommon -I. -pthread \
  record/07_queue_pool_consumer/queue_benchmark.cpp -o /tmp/old_queue_benchmark
```

队列报告单位是整窗耗时／消息数，`full` 则除以失败调用数；7 轮结果的中位数不是单消息 P50。满队模式每循环做两次失败调用，计数和业务数据都校验。

消费基准使用同样的真实普通日志文件，每批在计时外等待日志排空，检查零丢弃。除法后的批均值再统计 P50/P99；样本数自适应，以累计计时至少 200 ms 为准，不把计时外等待算进这一指标。回报模式只测用户态消费、编码和缓冲复制，不包括真实 send、审计刷盘或网络 RTT。

对象池基准可以直接运行 `pool_layout_benchmark TYPE WORKLOAD CAPACITY CYCLES RAW_CSV`。TYPE 支持 `me_order`、`market_order`、`me_level`、`market_level`、`market_update`；WORKLOAD 为 `reuse` 或 `random`。正式数量见 `pool_measurement.json`，校准与预分配不计时；随机 65,536 槽为压力场景。

`queue_semantics` 的 Logger 测试故意用容量 128 突发提交 20,000 条，允许诊断日志按既有规则丢弃，验证写入数＋丢弃数＝20,000 和无重复。它与消费性能探针的零丢弃条件不同。

真实交易引擎探针每批96条事件；正式每轮固定166批、15,936条计时事件，32批预热。计时包含 run 的普通日志、簿更新、特征和观察回调，装填队列、日志排空、构造和收尾在计时外；无网络和关键审计。其 JSON 分别标注整窗 mean 和批均值 P50/P99。

chain_measurement.json 是两档速率的28轮旧／新网络对照，aa_control_measurement.json 是相同当前程序的14轮波动校准，两者独立理解。业务均通过，网络 P50 未改善；新增延迟的具体分量尚未定位。
