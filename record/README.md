# 修复与优化记录

每项工作使用一个 `编号_工作` 目录。报告、用于复现问题的测试、性能测量脚本及结果放在同一目录，编号不包含日期。

| 编号 | 工作 | 报告 |
|---|---|---|
| 00 | 组播接口选择 | [00_multicast_interface](00_multicast_interface/00_multicast_interface.md) |
| 01 | 可靠性改造与逐项性能测量 | [01_reliability](01_reliability/01_reliability.md) |
| 02 | 可复现测量与基准口径修正 | [02_reproducible_measurement](02_reproducible_measurement/02_reproducible_measurement.md) |
| 03 | 性能调优与吞吐、尾延迟定位 | [03_performance_tuning](03_performance_tuning/03_performance_tuning.md) |

报告按“测试发现问题 → 寻找问题 → 解决问题 → 重新测试”展开。每项改造先保留性能基准，修改后验证业务结果，再用相同有效输入重新测量。原始失败记录不删除；耗时增加也要记录。

`*.journal` 是运行时审计或账户文件，留在本机，不提交到账户源码仓库。构建目录和普通日志也不提交。目录内的 JSON、CSV 和图表是可审查的测试摘要与测量证据。03 本次先提交报告与证据，候选代码及辅助实验源代码暂留工作区，不随报告提交。
