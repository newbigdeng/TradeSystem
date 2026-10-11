# 修复与优化记录

先读主报告：问题是什么、为什么、怎么改、测试结果如何。需要具体代码时再打开“源码对照”，点击对应问题展开。

| 编号 | 主报告 | 源码细节 |
|---|---|---|
| 00 | [组播接口](00_multicast_interface/00_multicast_interface.md) | [修改前后](00_multicast_interface/00_source_changes.md) |
| 01 | [可靠性改造](01_reliability/01_reliability.md) | [修改前后](01_reliability/01_source_changes.md) |
| 02 | [测量口径](02_reproducible_measurement/02_reproducible_measurement.md) | [修改前后](02_reproducible_measurement/02_source_changes.md) |
| 03 | [性能调优](03_performance_tuning/03_performance_tuning.md) | [修改前后](03_performance_tuning/03_source_changes.md) |
| 04 | [源码修复核对](04_source_repairs/04_source_repairs.md) | 汇总已有修复，补充验收 |
| 05 | [perf重新测量](05_perf_remeasurement/README.md) | 真实perf、队列单因素、批处理与容量验收 |
| 06 | [队列、对象池与定序器](06_queue_pool/06_queue_pool.md) | [修改前后](06_queue_pool/06_source_changes.md) |
| 07 | [端到端延迟排查与发送推进](07_e2e_latency/07_e2e_latency.md) | [修改前后](07_e2e_latency/07_source_changes.md)；含未采用候选和全部慢轮 |
| 08 | [整链延迟与运行环境](08_e2e_environment/08_e2e_environment.md) | 逐笔审计分段、宿主采样、索引对照；回报推进候选尚未采用 |

每项工作放在 `编号_工作` 目录，文件名不加日期。报告、回归工具和紧凑结果放在同一目录；日期、版本和验证范围写在正文。

修改生产实现时，先保存基准，每次只改一项，验证正确性后再用相同条件复测。慢轮、失败与性能成本均保留；不把业务通过写成所有性能门槛通过。

JSON、CSV 和图表保留原始结论。运行日志、审计文件、大样本及构建留在独立资料目录。03 计时配置、快照改造和实验源码已随 995bdca 推送；历史报告仍保留测量当时的冻结身份。

2026-10-06 的05重新测量公开报告、数值汇总和图表；既有候选补丁、实验源码、原始perf与逐请求数据保留在测量归档，可按固定提交复现。默认交易实现未切换为实验候选。
