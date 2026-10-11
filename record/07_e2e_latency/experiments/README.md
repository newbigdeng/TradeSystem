# 未采用的性能候选

这里的文件仅供复核，不参与默认构建。最终版本只采用纯发送刷新。

- candidate_network_test.cpp：4 帧接收预算和空批次抑制的阶段回归；依赖对应候选源码，不能直接当作最终版测试。
- receive-budget-and-guard.patch：发送修复之后，再加入接收预算和空批次判断的候选。
- guard-only.patch：仅保留空批次判断的隔离对照；还保留当时未使用的预算成员，按实际冻结源码理解。
- idle-sleep.patch / idle-yield.patch：两个空闲调度候选。
- sync_probe.c：fsync/fdatasync 独立诊断插桩，不用于正式计分或生产运行。

组名与版本字段的含义见主报告。正确性通过不表示性能通过；所有候选失败或变慢的结果仍保留。
