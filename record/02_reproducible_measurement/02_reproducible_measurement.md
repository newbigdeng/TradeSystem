# 02 测量口径：数字先要对应真实工作量

实现版本：`d78282c`；CPU 采样修正：`aff900f`。文档核对：2026-10-04。

修正了基准计数、计时和完成边界，并建立固定输入、逐条样本及审计对账。主批次 55,000 条正式请求，CPU 复测另有 10,000 条，全部收到预期回报。**业务通过，尾延迟门槛没有全部通过。**

## 测试发现了什么

- 内存池把分配和释放合计耗时，标成一次调用；“优化版”实际是同一类型的别名。
- 订单簿一次调用的总耗时额外除以 2；不消费输出，第二种实现因队列满退出。
- Logger 两次输入不同，固定等十秒，没有核对文件内容。
- 旧 TSC 读数未标定，却叫 CPU 周期。
- 新探针最初误把撤单的“成交数量不适用”字段当作 0。
- CPU 累计量从发送前采样，时间分母却从计划发送起点算起，漏掉一段等待。

原失败与逐项重测保留在 [diagnostic_cycles.json](diagnostic_cycles.json)。具体源码前后见 [02_source_changes.md](02_source_changes.md)。

## 怎样修正

组件计时改用 `steady_clock` 纳秒：分配、释放分别取样，订单簿每次调用只算一个样本。输入在计时前生成，两个实现用相同输入；Logger 排空后逐字节核对，撮合簿比较全部回报和行情，检查最终空簿及数量守恒。

外部生成器保存计划发送、第一次写入、完整发送和收到回报的时刻，保留短写进度及超时。开环按计划到达时刻发单，不等待上一条 ACK；闭环另做低在途量和存储诊断。

三个时间不能混用：

| 指标 | 实际含义 |
|---|---|
| 计划发送 → ACK | 包含生成器迟到、排队及交易所处理 |
| 完整发送 → ACK | 客户端观察到的请求往返 |
| 窗口内完成数 ÷ 窗口秒数 | 窗口吞吐，窗口后的回报不能补算进去 |

延迟样本仍按发起批次归属。窗口后才完成的请求保留真实延迟；有超时或未发送时，全部请求的精确 P99 标为不可估计。最近秩算法为 `sorted[ceil(p*n)-1]`；报告五轮各自 P99 的中位数和范围，不能平均 P99，也不删除慢轮。

CPU 另用时钟包围两次 `/proc` 计数采样，分子和分母对齐。修正的是统计口径，不是交易程序变快。

## 正式测量的结果

环境为 8 vCPU Ubuntu VM。服务允许 CPU 0–3，生成器 CPU 4；Release `-O3 -DNDEBUG`，审计逐条 fsync，主结果放 ext4。固定种子 20260928，NEW:CANCEL=1:1，数量 1、价格 100/356/612，不成交；每轮 1,000 请求，预热后独立测五轮。

固定 4 客户端、8 品种的容量探查如下。P99 是计划发送到 ACK 的五轮中位数，单位 ms。

| 目标请求/秒 | 到达窗口 | P99 | 窗口完成/秒中位数 | 20 ms 门槛 |
|---:|---:|---:|---:|---|
| 50 | 20 秒 | 25.251 | 50.0 | 并非五轮全通过 |
| 100 | 10 秒 | 46.760 | 100.0 | 并非五轮全通过 |
| 250 | 4 秒 | 4,285.592 | 59.0 | 积压 |
| 1,000 | 1 秒 | 7,030.902 | 0.0，范围 0–1 | 大量回报在窗口后 |

1,000 档最终收齐 1,000 条，也不能称窗口完成吞吐为 1,000/秒。完整客户端/品种矩阵、全部慢轮及原始摘要见 [summary.json](summary.json)、[network_runs.json](network_runs.json)。

同一程序的闭环存储对照，ext4 P99 为 13.855 ms，tmpfs 为 1.932 ms；输入和二进制相同。这说明审计对存储条件敏感，不是撮合代码优化。tmpfs 不提供相同的持久落盘条件。

CPU 口径修正后另做十轮复测：10,000 请求全通过，采样 CPU 为 328.1%–392.3%。这是所有线程合计，允许超过 100%；该批次与主批次分别保存，见 [resource_runs.json](resource_runs.json)、[resource_summary.json](resource_summary.json)。

## 怎样核查

Release 22 项 CTest 全通过；新增六项在 Debug、ASan/UBSan、TSan 下通过。主批次审计核对含预热共 71,376 条请求，最终无残单、无非零持仓，三个业务队列排空。详见 [validation_release.json](validation_release.json)、[validation_profiles.json](validation_profiles.json)。

运行入口：

```bash
python3 record/02_reproducible_measurement/run_measurements.py \
  --build build/reliability-release --output "$HOME/trade-measurements/new-batch"
```

正式批次要求工作树干净，保存源版本、编译参数、输入和二进制摘要。样本见 [sample_events.csv](sample_events.csv)，分析工具为 [analyze.py](analyze.py)。组件调用耗时、ACK 往返和真实成交延迟是不同指标，本轮没有测完整策略决策到成交链路。

<a id="source-comparison"></a>

8 项源码说明见 [02_source_changes.md](02_source_changes.md)。
