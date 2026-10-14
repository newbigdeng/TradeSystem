# 09｜性能 notebook 适配与统计修正

验证日期：2026-10-14。程序及脚本基线：`ce3c80a`。

**结论：旧 notebook 的路径、单位和配对方法需要修改。现在按实验目录读取数据，保留完整样本，复用现有请求分析器；TSC 未标定时显示 ticks，缺少请求 ID 的 TTT 只显示覆盖情况。虚拟机定向测试和实际数据重算通过，notebook 的七类输入初测与新增预热边界执行全部通过。**

## 测试时发现的问题

文件：[perf_analysis.ipynb](../notebooks/perf_analysis.ipynb)。旧版只扫描仓库上层的 `exchange*.log`、`*_1.log`，无法自然接上独立运行目录，也会漏掉其他客户 ID 的日志。

另外有三处会影响分析结论：

1. 固定使用 2.60 GHz 换算 RDTSC。TSC 计数不是标定后的纳秒，CPU 标称主频也不一定是 TSC 频率。
2. 去重、删除非正值，再裁掉 1% 与 99% 两端。相同记录可能是两次真实调用；裁剪还会去掉最需要观察的尾部，使慢样本从结果中消失。
3. 把相邻 TTT 行相减当成两阶段耗时。多请求、多线程的记录会交错，当前 TTT 又没有请求 ID，使用的还是 system_clock；相邻两行不能证明属于同一请求。

默认 Release 关闭诊断采样时，旧版空 DataFrame 还会因缺少 timestamp 列报错。多余的 hvplot、Plotly 等导入也增加了运行依赖。

## 寻找问题

对照 [perf_utils.h](../common/perf_utils.h)、[time_utils.h](../common/time_utils.h)、[组件输出](../tests/02_reproducible_measurement/benchmark_support.h)和[请求分析器](../tests/02_reproducible_measurement/analyze.py)：

- RDTSC 是原始计数差，未做频率标定或序列化计时保障。
- TTT 是墙钟纳秒时间戳，格式中只有时间、类型、标签和值。
- 当前组件基准会写 CSV；外部请求生成器另写 events.csv 和保存统计窗口的 result.json。
- 分位数、请求守恒和超时处理已经有严格分析器，应复用它，不再维护一套宽松的推测配对逻辑。

## 解决问题

### 数据路径和空输入

旧版入口：

```python
for filename in glob.glob("../exchange*.log") + glob.glob("../*_1.log"):
    ...
```

现在显式选择一次运行或测量的目录：

```python
DATA_DIR = os.environ.get("TRADE_ANALYSIS_DIR") or None
data = analysis.analyze_directory(DATA_DIR, TSC_HZ, REQUEST_WINDOW_NS)
```

文件、标签、版本与轮次分别列出，不自动合并。没有设置目录、只有关闭采样的普通日志时明确提示没有可分析数据，不当成零耗时。

### TSC 单位

旧版：

```python
CPU_FREQ = 2.60
latency_rdtsc = latency / CPU_FREQ
```

现在默认保留原始计数。只有显式提供实测 Hz 才转换：

```python
value if tsc_hz is None else value * 1e9 / tsc_hz
```

表格和坐标轴同时显示单位。没有标定值时显示 TSC ticks，不用 CPU 标称频率代替；即便填入频率，诊断日志也不等同于可靠的逐请求单调时钟计时。

### 保留慢样本，取消猜测配对

旧版会过滤掉两端样本：

```python
t_df = t_df[(t_df['latency'] < q_hi) & (t_df['latency'] > q_lo)]
```

现在统计和图表都保留全部合法样本，包括重复行、零值及最大值。非法值、格式错误、未知列和元数据不一致单独提示，不静默当作有效结果。

旧版对两个标签的混合记录执行：

```python
t_df['latency_diff'] = t_df['latency'].diff()
```

这段链路估算已移除。TTT 仅列条数和完整整数时间戳范围。真正的请求 RTT 复用 `analyze.py`：请求 ID 必须唯一，状态和时间合法；优先使用 result.json 的窗口，并核对重算结果。归档中的 warmup_events.csv 明确提示为预热，不纳入正式统计窗口，避免拿正式元数据核对预热记录。

TIMEOUT、UNSENT 单独列出。完成请求的 P99 不代替全部输入的 P99；有未完成项时，后者保持不可精确估计状态。原业务验收通过、失败或缺失也分别显示。

### 当前 CSV 和依赖

数值读取放在 [perf_analysis.py](../notebooks/perf_analysis.py)，只用标准库并复用原分析器。notebook 负责表格和图；移除不再使用的 pandas、NumPy、Plotly、hvplot、session_info 导入，图表使用可选的 matplotlib。

组件 `operation,elapsed_ns` 按每个样本解释，样本可能包含多个操作或一轮恢复。池的 `cycle,pairs,elapsed_ns` 按每周期平均每对成本解释。两者不混作单次操作延迟；分位数使用 nearest rank，历史探针的原统计结果继续保留。

## 重新测试

- 虚拟机 Python 3.14.4 第一轮 **15 项定向反例测试**通过，覆盖重复行、慢尾部、TSC 换算、TTT 精度、池分母、错误 CSV、超时／未发送及元数据不一致。补足预热边界后，仅复测新增预热、元数据失配、超时三项，全部通过，累计覆盖 16 个用例。测试代码位于 [09_notebook_analysis](09_notebook_analysis/test_analysis.py)。
- 读取虚拟机已有 7 轮、4 个组件程序的 **280 份组件 CSV**，计数与 P50/P95/P99/max 和各自 result.txt 一致。另有 7 份 book_input.csv 是订单输入，明确提示未纳入延迟样本。
- 从虚拟机取回实际文件的字节副本：8 份组件 CSV、池周期 CSV、两份共 **512 条正式请求**；请求重算与原 result.json 的完整 statistics 一致。原样本和慢轮未修改。
- 在本地独立 Python 3.12 测试环境启动真实 Jupyter kernel，执行七类输入初测：未配置目录、关闭采样日志、实际组件、实际请求、实际池周期、TSC／TTT 构造反例、超时／未发送构造反例。随后加入真实预热 CSV，仅复测请求场景。共 **8 次 notebook 执行、48 个代码单元**通过，6 次执行正常生成图表；无输入和关闭采样两次明确提示无计时样本。构造数据只验证边界，不作为性能成绩。
- notebook 格式验证、单元格编译、空输出及空执行编号检查通过。执行结果与图表留在实验归档，仓库保留可重新执行的源码。

本次修正的是分析方法，没有改变 C++ 业务代码或重新测量交易性能。第 7 篇“组件有效、订单网络往返 P50 未改善”的结论保持不变。

使用方法见 [notebooks/README.md](../notebooks/README.md)，根 README 与记录目录入口已同步。
