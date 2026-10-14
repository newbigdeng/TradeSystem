# 性能数据分析

`perf_analysis.ipynb` 读取当前程序生成的诊断日志和测量文件。先选一次实验，再分别查看组件与请求往返；每个来源单独统计，保留慢样本。

## 打开 notebook

用已有的 Python 内核打开 `perf_analysis.ipynb`。若需要 JupyterLab，可在独立 Python 环境安装 `jupyterlab`；图表另外需要 `matplotlib`。数值分析复用仓库的 `tests/02_reproducible_measurement/analyze.py`，不依赖 pandas、NumPy、Plotly、hvplot 或 session_info。

启动前设置一次实验的数据目录，例如：

```bash
TRADE_ANALYSIS_DIR=/path/to/component-results jupyter lab notebooks/perf_analysis.ipynb
```

也可以直接修改 notebook 的 `DATA_DIR`。联合启动和组件脚本都会打印输出目录，复制那个路径即可。需要分析逐请求数据时，指向包含 `events.csv`、`result.json` 的测量目录；多个子目录分别列出，不混合版本或轮次。

当前脚本测量入口是：

```bash
bash scripts/run_benchmarks.sh
TRADE_RANDOM_ORDERS=100 bash scripts/run_exchange_and_clients.sh 5 RANDOM
```

前者生成组件 CSV；后者生成普通日志与审计，**不会自动生成外部测量器的逐请求 RTT CSV**。已有逐请求测量数据可以直接分析，其输入和业务校验范围见 [07 报告](../record/07_book_recovery.md)。

只想看数值时无需安装 Jupyter：

```bash
python3 notebooks/perf_analysis.py /path/to/results
python3 record/09_notebook_analysis/test_analysis.py
```

## 数据含义

| 输入 | 读取方式与边界 |
|---|---|
| `*.log` 的 RDTSC 行 | 默认保留 TSC ticks。只有显式填写实测 `TSC_HZ` 才换算为 ns；CPU 标称 GHz 不是标定值 |
| `*.log` 的 TTT 行 | 保留数量和整数墙钟时间戳范围。没有请求 ID，不计算相邻阶段差值 |
| `operation,elapsed_ns` CSV | 程序定义的每样本纳秒计时，可能是一对入出队、一次缓冲复制或一个快照周期；含读钟开销 |
| `cycle,pairs,elapsed_ns` CSV | 除以每行 pairs，统计周期平均每对成本，不等于单对操作的延迟分布 |
| 请求 CSV 与 `result.json` | 复用现有严格分析器；重算请求 ID、状态、窗口和分位数，核对保存结果及验收标记 |

请求字段至少包括 `request_id,kind,scheduled_ns,sent_ns,completed_ns,status`，当前生成器还有 `first_write_ns`。`sent_ns` 是完整请求帧发出时间。只有没有元数据时才显式填写 `REQUEST_WINDOW_NS`；不会从首尾行推测统计窗口。

归档中的 `warmup_events.csv` 是预热请求，单独保留并提示，不纳入正式统计，也不和正式请求的 result.json 比较。

分位数使用 nearest rank，不删除 1%/99% 两端，不把重复计时行去重。当前组件有 `result.txt` 时会核对计数和分位数，未知格式明确提示。部分历史批次探针使用不同边界索引，新重算不覆盖原报告。

TIMEOUT、UNSENT 单独显示；存在这些状态时，所有输入的 P99 保留不可精确估计状态。表中的 RTT 分位数只描述完成请求，`correctness` 保留原业务验收通过、失败或未提供状态。输入窗口完成速率与窗口 cohort 最终完成量分开列出。

## 诊断采样

当前 Release 默认 `TRADE_PERF_TRACE_EVERY=0`。普通日志没有计时行时，notebook 会提示无数据，不将其解释为零耗时。

需要诊断记录时另建构建目录，避免覆盖正式测量版本。联合脚本会重新配置并增量构建，因此采样变量要附在联合启动命令上：

```bash
TRADE_BUILD_DIR=build/diagnostic TRADE_PERF_TRACE_EVERY=1 TRADE_RANDOM_ORDERS=100 \
  bash scripts/run_exchange_and_clients.sh 5 RANDOM
```

诊断记录会增加日志与格式化成本，应与关闭采样的性能成绩分开。TSC 日志未经标定、没有序列化计时保障，主要用于诊断，不能替代单调时钟逐请求测量。

notebook 按来源绘制完整样本和经验累计分布，不自动生成旧／新提速结论。提交到仓库的文件清空输出和执行编号，避免混入机器路径与旧图表。

本次修正及验收见 [09 报告](../record/09_notebook_analysis.md)。
