# 02｜可复现测量与基准口径修正

补充说明（2026-10-03）：本报告新增[逐项源码修改前后对照](#source-comparison)，原失败记录、测试结果和性能数字保持原样。

本次修正了三个基准程序的计数、计时和完成边界，并建立固定输入、开环负载、逐条样本及审计对账的测量流程。主批次完成 55 次正式网络运行，随后 CPU 口径修正再做 10 次正式复测，合计 65,000 个请求全部得到预期回报；同时保留了高负载积压和低负载尾延迟波动。

结论是：基准程序的问题已修复，测量过程可以复现；ext4 审计路径下的延迟毛刺仍存在。本轮没有改变交易引擎实现，不能把计时口径或存储条件变化写成引擎提速。

## 测试时发现问题

修改前先在 Ubuntu 虚拟机运行了三个原有基准，保留标准输出、退出码、二进制哈希和日志大小。订单簿基准运行第一个实现后，因回报队列满退出，退出码为 1。这轮是失败诊断，不能参与速度对照。

内存池和 Logger 的“优化版”当前都是原实现的别名。它们的两次耗时有波动，不表示算法优化。旧输出使用未经串行化及标定的 TSC 读数并称为 CPU 周期；订单簿每次只测一次调用，却将总耗时除以两倍调用数。内存池则把一次分配与一次释放合并，标成了单次调用。

原有端到端脚本串行等待 ACK，只保存汇总值。它适合低在途量的往返诊断，但缺少预定到达时间、原始逐条数据和窗口内完成数，无法说明过载时的积压。

实际运行还暴露了两个测量问题：一是新探针最初把撤单回报中“不适用”的成交数量字段当作 0，误报了协议错误；二是 CPU 百分比的分子包含发送前等待，分母却从计划发送起点计算，短轮会偏高。两项都保留了原始证据，修正后重新验证。

## 寻找问题

检查 `benchmarks` 中三个程序，以及订单簿的回报、行情出口。原程序连续灌入请求而不消费输出；更换实现后继续使用同一组队列，积累的消息最终触发正确的满队列保护。问题在基准驱动方式。

随后逐项确定测量边界：池分配、池释放、Logger 调用、订单簿调用到回报产生、跨线程队列传递、TCP 用户缓冲追加、快照组件构建、外部请求到 ACK。ACK 表示接受或撤单结果，不代表成交，也不包含完整交易客户端的行情应用链路。

通过 `findmnt` 又确认：`/tmp` 为 tmpfs，主结果目录位于 ext4。代码在 ACK 前同步写审计记录；内存文件系统上的 `fsync` 调用不代表物理存储落盘。此前临时目录中的数字与本轮 ext4 数字不能直接组成“版本提速”对照。

## 解决问题

1. 三个基准改用 C++ `steady_clock` 纳秒，每个样本只对应一次明确调用；内存池分开统计分配和释放。
2. 输入在计时前生成。Logger 同时覆盖短文本和长文本，等待排空后核对全部输出字节；订单簿用回调收集全部回报和行情，核对两种实现的事件、数量守恒及最终空簿，避免无人消费的队列。
3. 测量空计时器开销，不用未经验证的固定 GHz 换算时间，也不直接扣除开销来制造更小的数字。
4. 增加队列跨线程、TCP 用户缓冲和快照恢复组件测量。它们保留独立边界，不称为订单端到端延迟。
5. 增加固定种子的独立网络负载生成器，支持开环与闭环；保留部分发送进度，核对客户端、会话、请求序号、订单、回报 ID、数量和审计账本。
6. 增加严格的 CSV 分析器，分别计算计划请求批次和实际时间窗口的计数。最近秩分位数采用 `sorted[ceil(p*n)-1]`；超时与未发送单独计数，整体 P99 在删失情况下标记为无法精确估计。
7. 每轮保存 manifest、环境、实际编译命令、源码和二进制哈希、输入哈希、墙上起止时间、单调时间样本、退出码、队列水位、CPU、内存及系统 UDP 计数。原始日志和大样本位于仓库外的独立结果目录，目录内报告与紧凑摘要用于审查。
8. 按协议将撤单的成交数量校验为 `Qty_INVALID`，并用真实进程复测。CPU 则单独记录采样开始、结束和持续时间，让 CPU tick 差值与对应时间区间一致；修正前的派生百分比作废，原始 tick 数据保留。

## 重新测试与复现方法

```bash
cmake -S . -B build/reliability-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/reliability-release -j 4
ctest --test-dir build/reliability-release --output-on-failure
python3 record/02_reproducible_measurement/run_measurements.py \
  --build build/reliability-release --output "$HOME/trade-measurements/new-batch"
```

正式批次要求工作树干净。默认每配置预热一次，再独立运行五次；配置顺序每轮反转，两种组件实现的执行顺序也交替，减弱先后顺序和环境漂移。程序拒绝覆盖已有结果目录或占用他人进程的端口。

网络矩阵覆盖 1/2/4 个客户端、1/8 个品种、NEW:CANCEL=1:1 和固定非交叉价格；容量实验单独改变 offered rate。固定非交叉输入允许逐条核对 ACK 和撤单结果；成交正确性由订单簿组件的部分成交、多档成交、同价 FIFO 和价格碰撞用例验证。

本轮改变的是测量设施，没有提交交易引擎性能优化候选。旧 TSC 数字与新纳秒分布的边界、单位和输入不同，不能计算“引擎提速百分比”。前期可靠性基线与故障证据继续保留在 `01_reliability`；后续单热点优化应在当前正确性基线上使用同一测量器、同一输入与相同条件进行 B1/B2 成对实验。

### 每项工作都先测、再改、再测

| 工作 | 修改前的证据 | 修改后复测 | 判断 |
|---|---|---|---|
| 内存池计数与时钟 | 输出 579/572“CPU 周期”；实际分母对应分配加释放，且两种类型为别名 | 分配、释放各保留 256,000 个样本；逐轮核对空闲容量与 checksum，预热一轮后测五轮 | 单次调用口径修正；旧单位不可直接换算成纳秒 |
| Logger 完成边界 | 固定等待 10 秒；两次输入不同，未验证输出内容；类型也是别名 | 两者使用相同 2,000 条长短文本，每份正文 8,330,890 字节；析构排空后逐字节核对，五轮全部通过 | 可以确认完成了相同工作；别名之间的波动不是优化收益 |
| 订单簿驱动与分母 | 共享输出队列不消费，第二种实现触发满队列保护，退出码 1 | 每种实现预热 600 次调用、测量 6,000 次；回调收集并比较全部回报和行情，数量守恒、最终空簿，五轮全部通过 | 基准可以完整跑通；保留生产队列的满保护 |
| 网络负载与统计 | 旧闭环脚本缺少原始样本与目标到达时间 | 固定输入；开环、闭环、1/2/4 客户端及 1/8 品种；五轮独立运行并逐单对账 | 吞吐、排队和协调遗漏可以分开检查 |
| 撤单校验 | 冒烟探针把 `Qty_INVALID` 当作 0，真实进程复现失败 | 修正预期字段；六次小规模冒烟通过，主批次所有新增和撤单回报通过 | 修复探针假设错误，未修改业务协议 |
| CPU 时间区间 | 旧派生百分比最高约 408.7%，其时间分母少包含了发送前约 20 ms | 单独包围资源采样；同一输入在 ext4/tmpfs 下交替预热及五轮复测 | 旧百分比不能用作准确利用率；修正验证数据独立保存 |

前三项迭代阶段各保留一次完整预热和五次测量，处于工具改造的工作树中，归类为诊断与工具验收。随后先提交源码，再在干净工作树上运行正式批次。旧代码还在独立目录从 `6dc2856` 重新构建，保存完整构建日志，再次复现订单簿基准退出码 1。详见 [diagnostic_cycles.json](diagnostic_cycles.json)。

### 正式实验条件

| 项目 | 本次条件 |
|---|---|
| 源码 | 主批次 `d78282cac805f4eb3af7270707d8d3424415b715`；工作树干净；业务源码哈希与起点 `6dc2856` 完全一致 |
| 虚拟机 | VMware，8 vCPU，CPU 显示为 i7-13650HX，单 NUMA 节点；宿主机负载不可见 |
| 系统与构建 | Ubuntu 26.04，Linux 7.0.0-34，GCC 15.2.0，CMake 4.2.3，Release |
| 实际编译参数 | `-O3 -DNDEBUG -std=gnu++20 -Wall -Wextra -Werror -Wpedantic`；正式数据未启用 sanitizer |
| 时钟 | C++ `steady_clock`；外部探针 `perf_counter_ns`，同一 VM 的单调时钟；墙上时间仅作批次关联 |
| 放置 | 交换进程允许 CPU 0–3，负载生成器固定 CPU 4；组件允许 CPU 0–1，跨线程队列的生产者/消费者分别固定 0/1 |
| 网络与缓冲 | `lo`，MTU 65536，TCP_NODELAY；生产 TCP 用户缓冲 1 MiB，主要业务队列容量 262144 |
| 日志与审计 | 原有诊断日志保持开启，持久审计仍逐记录写入并 `fsync`；每轮使用新目录 |
| 输入 | 种子 20260928；每轮 1,000 个请求，NEW:CANCEL=1:1，BUY，数量 1，价格为 100/356/612，非交叉负载 |
| 预热与重复 | 每次进程先完成每客户端 32 个预热请求；每配置另丢弃一整轮预热，再独立测五轮；所有预热数据也保留 |
| 观察边界 | 开环按预定到达时刻发单；到达窗结束后最多继续观察 30 秒，晚完成保留，窗口外回报不计入窗口吞吐 |
| 预先声明的容量预算 | 每轮正确、计划到达→回报 P99 ≤20 ms，且至少 98% 请求在到达窗内完成；这是实验预算，不是已达到的服务承诺 |

空计时器也逐程序采样，P50 约 59 ns。这个开销对纳秒级微基准不可忽略，因此下表保留原始测量值，没有直接减去空调用，也没有把它当成纯算法成本。引擎原有 TSC 与墙上时间诊断日志未用于本报告的耗时换算。

### 微基准与组件结果

下表单位为 **µs**，各格是五轮对应指标的中位数；“最大值”也是各轮最大值的中位数，实际单轮数据与全部极端样本保留在原始 CSV。组件吞吐包含该驱动的校验和循环开销，不能外推为订单端到端吞吐。

| 指标 | P50 | P95 | P99 | 最大值中位数 | 测量边界 |
|---|---:|---:|---:|---:|---|
| 池分配 A | 0.105 | 0.121 | 0.133 | 334.413 | 单次 allocate 调用 |
| 池释放 A | 0.142 | 0.185 | 0.191 | 582.075 | 单次 deallocate 调用 |
| Logger A | 4.386 | 8.606 | 29.701 | 267.383 | log 调用返回；完成吞吐另包含排空和关闭文件，不保证 fsync |
| Logger B（同一别名） | 4.894 | 33.057 | 74.177 | 269.689 | 同上，输入完全相同 |
| 主订单簿 | 11.875 | 57.310 | 121.062 | 874.955 | book 调用→全部回调事件产生，含日志与校验回调 |
| 另一订单簿实现 | 12.283 | 57.913 | 110.719 | 1084.839 | 相同请求和回报序列 |
| 同线程队列 push+pop | 0.067 | 0.068 | 0.069 | 95.525 | 一组 push+pop，不称为单次调用 |
| 跨线程队列 | 3.055 | 235.928 | 402.876 | 509.221 | 成功 push 尝试前的时间戳→消费者取到；包含排队和调度 |
| TCP 缓冲追加 | 0.086 | 0.104 | 0.119 | 214.306 | 512 字节追加至用户缓冲；没有调用内核发送 |
| 快照恢复组件 | 55.594 | 114.323 | 238.629 | 701.211 | 74 帧、8 品种、64 个订单的构建和组件提交；不含网络等快照时间及 TradeEngine 应用 |

两个订单簿的每一条回报和行情事件都相同。场景覆盖空簿新增、同价 FIFO、部分成交、多档成交、撤单成功/失败及 100/356 价格碰撞；最大同时在簿订单数为 3。两种 Logger 及内存池为同一实现的别名，所以表中的差值是实验波动，不是两种算法的收益。详见 [components.json](components.json)。

### 真实交换进程：客户端和品种矩阵

这张表测量外部生成器的**计划请求时间→探针解码目标 ACK**。开环每轮 offered rate 为 100 个订单请求/秒，持续 10 秒。一次 NEW 和一次 CANCEL 各算一个请求；接受不等于成交。延迟单位为 **ms**，吞吐单位为**窗口内完成的请求/秒**。

| 客户端/品种 | P50 | P95 | P99 | 各轮 P99 范围 | 全部样本最大值 | 窗口完成吞吐中位数 |
|---|---:|---:|---:|---|---:|---:|
| 1 / 1 | 9.375 | 46.777 | 125.049 | 68.081–529.614 | 591.357 | 100.0 |
| 1 / 8 | 10.284 | 74.625 | 147.165 | 96.019–287.911 | 377.852 | 99.9 |
| 2 / 1 | 8.813 | 66.849 | 135.024 | 103.728–657.110 | 706.771 | 99.9 |
| 2 / 8 | 9.229 | 34.686 | 128.004 | 85.693–2597.888 | 2629.214 | 100.0 |
| 4 / 1 | 9.327 | 20.369 | 126.995 | 94.025–134.592 | 204.218 | 100.0 |
| 4 / 8 | 8.917 | 17.991 | 46.760 | 14.125–125.223 | 180.775 | 100.0 |

表中 P99 是“各轮 P99 的中位数”，不是合并 5,000 条样本后的 P99。2 客户端/8 品种有一轮 P99 达到 2.598 秒，该轮窗口完成吞吐降至 74.1；它没有被删去或改标为无效。每轮仅约十条样本落在 P99 之后，跨轮波动必须一起看。

### 容量实验：发得出不等于处理得完

这里固定 4 客户端/8 品种，每轮仍为 1,000 个请求，只改变目标到达速率。最终所有请求都收到回报，但高负载的大部分完成发生在到达窗之外。

| Offered rate | 到达窗 | P50（ms） | P95（ms） | P99（ms） | 全部样本最大值（ms） | 窗口完成吞吐中位数 | 预先声明的预算 |
|---:|---:|---:|---:|---:|---:|---:|---|
| 50 | 20 秒 | 8.950 | 12.401 | 25.251 | 106.624 | 50.0 | 五轮中有超出 20 ms 的轮次，不通过 |
| 250 | 4 秒 | 3411.190 | 4206.762 | 4285.592 | 6453.750 | 59.0 | 明显积压，不通过 |
| 1000 | 1 秒 | 4803.258 | 6918.629 | 7030.902 | 7496.917 | 0.0（范围 0–1） | 明显积压，不通过 |

1000 条/秒那一秒内几乎没有完整 ACK 被探针观察到，随后排空才收齐 1000 条。因此不能把最终完成数除以一秒，称为 1000 条/秒完成吞吐。在所测离散速率下，尚未确认满足上述尾延迟预算的最大稳定容量；这不等于系统完全不能处理请求。

### 存储对照：相同程序，不同持久化条件

使用相同二进制、相同输入哈希、相同 CPU 放置，交替执行 ext4 与 tmpfs 闭环，五轮逐对保存。闭环等上一条 ACK 再发下一条，用于这一存储条件对照；它不能替代固定 offered rate 的开环容量实验。

| 运行目录文件系统 | P50（ms） | P95（ms） | P99（ms） | 各轮 P99 范围 | 全部样本最大值（ms） | 闭环完成吞吐中位数 |
|---|---:|---:|---:|---|---:|---:|
| ext4 | 7.731 | 10.080 | 13.855 | 11.050–19.233 | 86.684 | 123.30 |
| tmpfs | 0.519 | 1.031 | 1.932 | 1.508–2.074 | 2.716 | 1654.70 |

这证明本测量对审计文件的存储条件显著敏感，不证明撮合代码变快。tmpfs 的小数字不能作为持久交易审计的落盘性能。源码中 ACK 前的审计写入与 `fsync` 是后续排查的重要对象，但本次没有内部各段耗时或系统调用追踪，不能将所有毛刺都归因于某一个函数。详见 [storage_pairs.json](storage_pairs.json)。

### 正确性、资源和验收

- 主批次共 66 次独立进程运行：11 次整轮预热、55 次正式测量。正式生成=发送=完成=55,000，其中 ACCEPTED 27,500、CANCELED 27,500，拒绝、超时、未发送均为 0。
- 连同进程内预热请求，共核对 71,376 条 RECEIVED、APPLY 与 RESPONSE。每轮无重复回报、未处理请求、残留订单或非零持仓；停止退出码均为 0，三个业务队列全部排空。
- 进程生命周期峰值 RSS 为 955,364 KiB，约 933 MiB；队列最高水位分别为请求 960、回报 124、行情 9。诊断日志丢弃计数为 0。
- 可见的系统 steal tick 增量为 0，UDP InErrors/RcvbufErrors/SndbufErrors 增量为 0。这些是 VM 内可见计数，不能证明宿主机没有抢占；网络实验没有订阅行情，不能据此声称完整行情链路无丢包。
- Release 最终 22 项 CTest 全部通过；新增 6 项在 Debug、ASan/UBSan、TSan 下均通过。生成器另用真实 socketpair 强制 3 字节短写和 7 字节分段响应，验证完整帧、固定输入、发送端停顿及超时保留；分析器验证最近秩、窗口与批次分离、重复 ID、时间倒序、删失和慢样本保留。

完整记录见 [network_runs.json](network_runs.json)、[aggregate.json](aggregate.json)、[validation_release.json](validation_release.json) 和 [validation_profiles.json](validation_profiles.json)。主批次旧 CPU 派生百分比因区间不一致已标记作废；原始计数保留，修正后验证另存，避免篡改历史数据。

CPU 口径修正固定在 `aff900fdabd9bf2b46138e3ddf6db48d3e1961e1`。新增验证程序 [verify_resource_timing.py](verify_resource_timing.py) 在干净工作树下又执行两次整轮预热、十次正式闭环测量，合计 10,000 个正式请求全部对账通过。每轮断言资源采样起止时刻包围完整测量，并核对 `CPU百分比 × 采样秒数 / 100 = CPU累计秒数`。修正后的全部线程 CPU 百分比约为 328.1%–392.3%，四个可用核合计可以超过 100%；分子仍是 `/proc` 的粗粒度 tick，不能解释为精确的瞬时使用率。

这次复测的 ext4/tmpfs P99 中位数分别为 14.954/2.068 ms，完成吞吐分别为 108.72/1394.92 请求/秒。它们与主批次分开保存，不选择较快批次替换原结果，也不称为优化收益。交易二进制和固定输入没有变化，修复的是资源指标的时间边界。数据见 [resource_manifest.json](resource_manifest.json)、[resource_runs.json](resource_runs.json) 和 [resource_summary.json](resource_summary.json)。

### 数据在哪里，如何复算

| 文件 | 用途 |
|---|---|
| [manifest.json](manifest.json) | 主批次源码、环境、编译、输入协议与测量哈希 |
| [summary.json](summary.json) | 每配置五轮分位数与吞吐的中位数和范围 |
| [sample_events.csv](sample_events.csv)、[sample_input.json](sample_input.json)、[sample_run.json](sample_run.json) | 一轮实际输入与逐条样本；CSV 仅将换行规范为 LF，原样本哈希仍保留 |
| [artifact_index.json](artifact_index.json) | 完整原始数据、构建日志和持久归档位置及索引 |
| `analyze.py`、`run_measurements.py`、`load_generator.py` | 分析与独立负载生成代码，和本报告在同一目录 |

完整主批次保留在 VM 的 `/home/zjh/trade-measurements/02/formal_d78282c`。tmpfs 对照数据另复制到该目录下的 `storage_controls_archived`，重启后仍可检查；原始日志、审计文件和大批量 CSV 不提交源码仓库。目录中的紧凑 JSON 保留全部运行，包括预热和失败诊断，不只展示最好的一轮。

CPU 修正复测保留在 `/home/zjh/trade-measurements/02/cpu_boundary_validation`，其 tmpfs 文件也归档至该目录的 `tmpfs_control_archived`。主批次、资源复测及仓库中的示例共 79 份 CSV 都已重新计算，计数与分位数和记录完全一致。发布文件的 SHA-256 见 [publication_hashes.json](publication_hashes.json)。

从 `sample_run.json` 的 `statistics.window_start_ns/window_end_ns` 取两个整数，然后运行：

```bash
python3 record/02_reproducible_measurement/analyze.py \
  record/02_reproducible_measurement/sample_events.csv \
  --start-ns <window_start_ns> --end-ns <window_end_ns>
```

本次没有测量完整交易客户端的决策到成交时间、行情发布到客户端应用时间或真实网络缺包恢复时间。组件恢复时间与 ACK 往返都有明确边界；后续报告应继续沿用这些边界，不将不同指标拼成一个“交易延迟”。

<a id="source-comparison"></a>

## 源码修改前后对照

以下新增代码块直接摘自对应提交或本次核实的工作区，保留真实代码，仅将换行统一为 LF。每个块标注文件、版本和源文件行号；它们是关键片段，不是可独立编译的完整文件。历史“修改后”指该项修复提交时的实现，后续改动另列。新增功能明确说明原来没有相同实现；缺失的未提交旧源码不根据记忆重建。完整改动可通过所列历史版本和提交链接检查。

| 对照 | 内容 |
|---|---|
| 01 | [内存池分配与释放分别计时](#source-01) |
| 02 | [Logger 输入相同，并验证真正排空的结果](#source-02) |
| 03 | [订单簿基准消费并比较全部输出](#source-03) |
| 04 | [新增开环生成器，保留计划时间和短写进度](#source-04) |
| 05 | [撤单回报的成交数量按“不适用”检查](#source-05) |
| 06 | [新增分析器，将迟到回报与窗口吞吐分开](#source-06) |
| 07 | [CPU 累计量和时间分母使用同一采样区间](#source-07) |
| 08 | [脚本入口使用统一、可核查的测量流程](#source-08) |

<a id="source-01"></a>

### 源码对照 01：内存池分配与释放分别计时

旧 total_rdtsc 同时累加 allocate 和 deallocate，却只除以对象个数，并将未经标定的 TSC 读数标成 CPU 周期。

**修改前**

[`benchmarks/release_benchmark.cpp`，`6dc2856`，第 17–17 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/release_benchmark.cpp#L17)

```cpp
      total_rdtsc += (Common::rdtsc() - start);
```

[`benchmarks/release_benchmark.cpp`，`6dc2856`，第 21–22 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/release_benchmark.cpp#L21)

```cpp
      mem_pool->deallocate(allocated_objs[j]);
      total_rdtsc += (Common::rdtsc() - start);
```

[`benchmarks/release_benchmark.cpp`，`6dc2856`，第 26–26 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/release_benchmark.cpp#L26)

```cpp
  return (total_rdtsc / (loop_count * allocated_objs.size()));
```


**修改后**

[`record/02_reproducible_measurement/benchmark_support.h`，`d78282c`，第 12–15 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/benchmark_support.h#L12)

```cpp
using Clock=std::chrono::steady_clock;
inline uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
```

[`benchmarks/release_benchmark.cpp`，`d78282c`，第 14–24 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/benchmarks/release_benchmark.cpp#L14)

```cpp
    for(auto& object:objects) {
      const auto start=Measure::now();object=pool.allocate();const auto ns=Measure::now()-start;
      Measure::require(object!=nullptr,"pool allocation failed");object->seq_num_=round;
      if(round>=warmup)allocate[index++]=ns;
    }
    if(round>=warmup)index-=objects.size();
    for(auto* object:objects) {
      checksum+=object->seq_num_;
      const auto start=Measure::now();pool.deallocate(object);const auto ns=Measure::now()-start;
      if(round>=warmup)release[index++]=ns;
    }
```


**为什么这样改，以及如何验证：** 改用 steady_clock 纳秒，分配与释放各保存一次调用的样本，预热不混入正式分布；校验容量及 checksum。原有类型为别名，A/B 只作重复控制。见 [diagnostic_cycles.json](diagnostic_cycles.json) 与 [components.json](components.json)。

<a id="source-02"></a>

### 源码对照 02：Logger 输入相同，并验证真正排空的结果

旧两个实例先后生成不同随机文本，调用耗时后固定等十秒，没有逐字节确认输出。

**修改前**

[`benchmarks/logger_benchmark.cpp`，`6dc2856`，第 23–26 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/logger_benchmark.cpp#L23)

```cpp
    const auto s = random_string(128);
    const auto start = Common::rdtsc();
    logger->log("%\n", s);
    total_rdtsc += (Common::rdtsc() - start);
```

[`benchmarks/logger_benchmark.cpp`，`6dc2856`，第 39–39 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/logger_benchmark.cpp#L39)

```cpp
    std::this_thread::sleep_for(10s);
```


**修改后**

[`benchmarks/logger_benchmark.cpp`，`d78282c`，第 14–25 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/benchmarks/logger_benchmark.cpp#L14)

```cpp
    for(size_t i=0;i<input.size();++i) {
      const auto start=Measure::now();logger.log("%\n",input[i]);samples[i]=Measure::now()-start;
    }
    dropped=logger.dropped_records();
  } // Join and drain; completion rate includes file close, but does not imply fsync.
  const auto elapsed=Measure::now()-window;
  std::ifstream file(name+".log");
  const std::string actual((std::istreambuf_iterator<char>(file)),{});
  Measure::require(dropped==0,"logger dropped records; invalid performance run");
  Measure::require(actual==expected+"[LOGGER] dropped_records=0\n","logger output bytes differ");
  Measure::output(name,samples,elapsed,expected.size(),dropped);
}
```

[`benchmarks/logger_benchmark.cpp`，`d78282c`，第 29–29 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/benchmarks/logger_benchmark.cpp#L29)

```cpp
    for(size_t i=0;i<2000;++i)input.push_back(std::to_string(i)+":"+std::string(i%2?8192:128,char('A'+i%26)));
```


**为什么这样改，以及如何验证：** 输入在测量前生成，两个别名实现共用相同短/长文本；作用域结束时排空并关闭文件，检查 dropped=0 和正文逐字节相同。log 调用延迟与整体排空完成速率分开解释，文件关闭不等于 fsync。原有结果和复测均保留。

<a id="source-03"></a>

### 源码对照 03：订单簿基准消费并比较全部输出

旧驱动通过同一个 MatchingEngine 共用输出队列，不消费回报和行情；第二种实现触发满队列保护。此外，一次调用的耗时除以了两倍调用数。

**修改前**

[`benchmarks/hash_benchmark.cpp`，`6dc2856`，第 33–33 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/hash_benchmark.cpp#L33)

```cpp
  return (total_rdtsc / (loop_count * 2));
```

[`benchmarks/hash_benchmark.cpp`，`6dc2856`，第 41–43 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/benchmarks/hash_benchmark.cpp#L41)

```cpp
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);
  auto matching_engine = new Exchange::MatchingEngine(&client_requests, &client_responses, &market_updates);
```


**修改后**

[`benchmarks/hash_benchmark.cpp`，`d78282c`，第 30–37 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/benchmarks/hash_benchmark.cpp#L30)

```cpp
  book.response_sink_=[&](const MEClientResponse& r) {
    result.responses.push_back(r.toString());
    accepted+=r.type_==ClientResponseType::ACCEPTED;canceled+=r.type_==ClientResponseType::CANCELED;
    rejected+=r.type_==ClientResponseType::CANCEL_REJECTED;filled+=r.type_==ClientResponseType::FILLED;
    if(r.type_==ClientResponseType::FILLED){volume+=r.exec_qty_;balance+=int64_t(r.exec_qty_)*sideToValue(r.side_);}
  };
  book.market_sink_=[&](const MEMarketUpdate& u){result.updates.push_back(u.toString());};
  std::vector<uint64_t> all(6000);std::array<std::vector<uint64_t>,12> cases;
```

[`benchmarks/hash_benchmark.cpp`，`d78282c`，第 42–47 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/benchmarks/hash_benchmark.cpp#L42)

```cpp
    const auto& r=rows[i];const auto start=Measure::now();
    if(r.type_==ClientRequestType::NEW)book.add(r.client_id_,r.order_id_,0,r.side_,r.price_,r.qty_);
    else book.cancel(r.client_id_,r.order_id_,0);
    const auto ns=Measure::now()-start;
    if(i>=600){all[i-600]=ns;cases[i%12].push_back(ns);}
    if(i%12==11)Measure::require(book.liveOrders().empty(),"scenario left live orders");
```

[`benchmarks/hash_benchmark.cpp`，`d78282c`，第 73–73 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/benchmarks/hash_benchmark.cpp#L73)

```cpp
    Measure::require(first.responses==second.responses && first.updates==second.updates,"implementations produced different business events");
```


**为什么这样改，以及如何验证：** 每样本明确对应一次 add/cancel，用回调收集输出，比较两种实现全部业务事件，逐周期确认空簿。保留生产满队列保护。见 [diagnostic_cycles.json](diagnostic_cycles.json) 中旧退出码 1 和修正后的五轮结果；没有把失效的旧工作量用于提速比例。

<a id="source-04"></a>

### 源码对照 04：新增开环生成器，保留计划时间和短写进度

旧端到端工具逐条发送并等待 ACK，适合闭环诊断，缺少预定到达时刻及逐条原始样本。这里增加了新的测量器，旧闭环工具仍保留。

**修改前**

该 load_generator.py 在父版本中不存在，不能给出同一文件的“旧函数”。旧闭环工具未被删除，也不把新增开环能力描述为修复了撮合算法。



**修改后**

[`record/02_reproducible_measurement/load_generator.py`，`d78282c`，第 60–74 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/load_generator.py#L60)

```python
def drive(peers, inputs, mode, rate, timeout_s, pause_ms=0):
    """Retains short writes; open-loop schedule is independent of acknowledgements."""
    selector = selectors.DefaultSelector()
    for peer in peers.values():
        selector.register(peer.socket, selectors.EVENT_READ, peer)
    rows = [{'request_id': f"{r['client']}:{r['order']}:{r['kind']}",
             'kind': 'NEW' if r['kind'] == 1 else 'CANCEL', 'scheduled_ns': 0,
             'first_write_ns': '', 'sent_ns': '', 'completed_ns': '', 'status': 'UNSENT'} for r in inputs]
    start = time.perf_counter_ns() + 20_000_000
    for row in rows:
        row['scheduled_ns'] = start
    interval = 1e9 / rate if mode == 'open' else 0
    if mode == 'open':
        for i, row in enumerate(rows):
            row['scheduled_ns'] = start + int(i * interval)
```

[`record/02_reproducible_measurement/load_generator.py`，`d78282c`，第 133–137 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/load_generator.py#L133)

```python
                        last_send = row['sent_ns'] = time.perf_counter_ns()
                        row['status'] = 'TIMEOUT'  # Unknown until an observed terminal response.
                        peer.pending.append((row, source, seq))
                        peer.outgoing.popleft()
                    if not peer.outgoing:
```


**为什么这样改，以及如何验证：** 新 drive 的计划时刻独立于 ACK；部分发送保留帧进度，完整发送才写 sent_ns，收到核验回报才写 completed_ns。新增生成器与分析器测试覆盖短写、拆包、停顿及超时。旧工具见 [e2e_benchmark.py](../01_reliability/e2e_benchmark.py)；本轮正式请求与审计数量见原报告验收部分。

<a id="source-05"></a>

### 源码对照 05：撤单回报的成交数量按“不适用”检查

首次冒烟误把撤单 exec_qty 的预期设为 0；真实协议返回 Qty_INVALID，即 0xFFFFFFFF。失败来自探针假设，而不是交易所协议需要改变。

**修改前**

修正前脚本当时尚未提交，其完整旧源码没有保留在可核实的 Git 版本中；保留了失败回包和 RuntimeError。此处不根据记忆拼出旧代码，只展示可核实的修正后实现与失败证据。



**修改后**

[`record/02_reproducible_measurement/load_generator.py`，`d78282c`，第 164–168 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/load_generator.py#L164)

```python
                        expected_exec = 0 if wanted == 1 else 0xFFFFFFFF  # CANCEL execution quantity is not applicable.
                        if reply[5] != wanted or reply[14] != 0 or reply[12] != expected_exec or reply[13] != source['qty']:
                            raise RuntimeError(f'unexpected business response: {reply}')
                        row['completed_ns'] = timestamp
                        row['status'] = 'ACCEPTED' if wanted == 1 else 'CANCELED'
```


**为什么这样改，以及如何验证：** NEW 期望 exec_qty=0，CANCEL 期望 0xFFFFFFFF，同时检查回报类型、拒绝原因和剩余量。见 [diagnostic_cycles.json](diagnostic_cycles.json) 的 smoke_failure 与后续全部业务复测。

<a id="source-06"></a>

### 源码对照 06：新增分析器，将迟到回报与窗口吞吐分开

原闭环工具只有汇总结果；新分析器需要保留所有发起请求的迟到结果，同时只把实际落在窗口内的完成计入窗口吞吐。

**修改前**

父版本中没有 analyze.py；这是新增统计实现，并非某个已有 P99 函数的替换。



**修改后**

[`record/02_reproducible_measurement/analyze.py`，`d78282c`，第 13–19 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/analyze.py#L13)

```python
def distribution(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return {'n': len(values), **{name: values[math.ceil(p * len(values)) - 1]
                               for name, p in [('p50_ns', .5), ('p95_ns', .95), ('p99_ns', .99)]},
            'max_ns': values[-1], 'mean_ns': sum(values) / len(values)}
```

[`record/02_reproducible_measurement/analyze.py`，`d78282c`，第 52–65 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/analyze.py#L52)

```python
            if completed is not None and start_ns <= completed < end_ns:
                windows['completed'] += 1
                windows[status.lower()] += 1
            if not start_ns <= scheduled < end_ns:
                continue
            windows['offered'] += 1
            cohort[status] += 1
            if sent is not None:
                late.append(sent - scheduled)
            if completed is not None:
                delays.append(completed - scheduled)
                rtt.append(completed - sent)
                kinds.setdefault(row['kind'], []).append(completed - scheduled)
    seconds = (end_ns - start_ns) / 1e9
```

[`record/02_reproducible_measurement/analyze.py`，`d78282c`，第 77–78 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/analyze.py#L77)

```python
            'all_offered_p99_ns': None if censored else distribution(delays).get('p99_ns'),
            'all_offered_p99_status': 'right-censored or unsent; not exactly estimable' if censored else 'observed'}
```


**为什么这样改，以及如何验证：** 分位数按最近秩计算；计划属于批次的请求即使窗口后才完成也保留其延迟，有超时/未发送时不伪造全部请求 P99。对应严格边界测试见 [analysis_test.py](analysis_test.py)，高负载积压结论仍保留。

<a id="source-07"></a>

### 源码对照 07：CPU 累计量和时间分母使用同一采样区间

原 /proc CPU tick 分子从 drive 前采样，时间分母却从 drive 里稍后的计划起点算起，遗漏发送前等待，短轮百分比偏高。

**修改前**

[`record/02_reproducible_measurement/load_generator.py`，`d78282c`，第 254–257 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/load_generator.py#L254)

```python
        before, resource_before = counters(), resources(process.pid)
        rows, start, end, counts = drive(peers, source, config['mode'], config['rate'],
                                        config.get('timeout_s', 5), config.get('pause_ms', 0))
        after, resource_after = counters(), resources(process.pid)
```

[`record/02_reproducible_measurement/load_generator.py`，`d78282c`，第 271–273 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/load_generator.py#L271)

```python
        metadata['measurement_and_drain_seconds'] = (counts['observed_end_ns'] - start) / 1e9
        metadata['process_cpu_seconds_measure_and_drain'] = (resource_after['cpu_ticks'] - resource_before['cpu_ticks']) / os.sysconf('SC_CLK_TCK')
        metadata['process_cpu_percent_measure_and_drain'] = 100 * metadata['process_cpu_seconds_measure_and_drain'] / metadata['measurement_and_drain_seconds']
```


**修改后**

[`record/02_reproducible_measurement/load_generator.py`，`aff900f`，第 254–261 行](https://github.com/newbigdeng/TradeSystem/blob/aff900fdabd9bf2b46138e3ddf6db48d3e1961e1/record/02_reproducible_measurement/load_generator.py#L254)

```python
        cpu_sample_start = time.perf_counter_ns()
        resource_before, before = resources(process.pid), counters()
        rows, start, end, counts = drive(peers, source, config['mode'], config['rate'],
                                        config.get('timeout_s', 5), config.get('pause_ms', 0))
        resource_after = resources(process.pid)
        cpu_sample_end = time.perf_counter_ns()
        after = counters()
        with (directory / 'events.csv').open('w', newline='') as handle:
```

[`record/02_reproducible_measurement/load_generator.py`，`aff900f`，第 275–280 行](https://github.com/newbigdeng/TradeSystem/blob/aff900fdabd9bf2b46138e3ddf6db48d3e1961e1/record/02_reproducible_measurement/load_generator.py#L275)

```python
        metadata['cpu_sample_start_ns'] = cpu_sample_start
        metadata['cpu_sample_end_ns'] = cpu_sample_end
        metadata['cpu_sample_seconds'] = (cpu_sample_end - cpu_sample_start) / 1e9
        metadata['process_cpu_seconds_sampled'] = (resource_after['cpu_ticks'] - resource_before['cpu_ticks']) / os.sysconf('SC_CLK_TCK')
        metadata['process_cpu_percent_sampled'] = 100 * metadata['process_cpu_seconds_sampled'] / metadata['cpu_sample_seconds']
        metadata['cpu_scope'] = 'all child threads; coarse /proc ticks; bracketed resource sampling includes presend wait and drain'
```


**为什么这样改，以及如何验证：** 时钟包围资源计数的两个采样点，CPU 百分比分母改用 cpu_sample_seconds。旧派生百分比作废但原 tick 保留。见 [verify_resource_timing.py](verify_resource_timing.py)、[resource_runs.json](resource_runs.json)；复测十轮 10,000 请求对账通过，仍是粗粒度采样。

<a id="source-08"></a>

### 源码对照 08：脚本入口使用统一、可核查的测量流程

旧 shell 入口逐个调用三个程序，缺少统一清单、重复轮次和工作树版本约束。

**修改前**

[`scripts/run_benchmarks.sh`，`6dc2856`，第 3–3 行](https://github.com/newbigdeng/TradeSystem/blob/6dc2856e66dc78894105ca25895c3780806fce9d/scripts/run_benchmarks.sh#L3)

```bash
bash scripts/build.sh
```


**修改后**

[`scripts/run_benchmarks.sh`，`d78282c`，第 1–10 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/scripts/run_benchmarks.sh#L1)

```bash
#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_root"
build_directory="${TRADE_BENCH_BUILD:-build/release}"
output_directory="${1:-$HOME/trade-measurements/$(date -u +%Y%m%dT%H%M%SZ)}"
cmake -S . -B "$build_directory" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_directory" -j 4
python3 record/02_reproducible_measurement/run_measurements.py \
  --build "$build_directory" --output "$output_directory"
```

[`record/02_reproducible_measurement/run_measurements.py`，`d78282c`，第 105–106 行](https://github.com/newbigdeng/TradeSystem/blob/d78282cac805f4eb3af7270707d8d3424415b715/record/02_reproducible_measurement/run_measurements.py#L105)

```python
    if data['dirty_tree'] and not args.diagnostic:
        parser.error('dirty tree cannot be a formal baseline; commit changes first')
```


**为什么这样改，以及如何验证：** 入口构建后调用统一测量器，正式批次要求工作树干净，保存源码/二进制/输入/环境摘要并交替配置次序。原本三个基准目标仍参与组件验收，未把整套脚本新增当作引擎性能优化。

### 完整提交与配套测试

正文聚焦产生问题和改变行为的关键源码；同次提交的调用方迁移、类型定义、构建配置及新增回归测试在以下完整提交中保留。新增测试或工具没有旧实现，不为它们虚构“修改前代码”。

- [`d78282c`：Correct benchmark accounting and add reproducible measurement protocol](https://github.com/newbigdeng/TradeSystem/commit/d78282cac805f4eb3af7270707d8d3424415b715)
- [`aff900f`：Align process CPU counter interval with its elapsed-time denominator](https://github.com/newbigdeng/TradeSystem/commit/aff900fdabd9bf2b46138e3ddf6db48d3e1961e1)
