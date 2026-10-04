# 02 源码对照

[返回主报告](02_reproducible_measurement.md)。点击具体问题展开代码。

片段取自实际 Git 版本或已核实的工作区，标注文件、版本及行号。片段省略上下文；完整实现见对应版本链接。新增功能注明原来没有该实现，未保存的旧辅助源码注明缺失。

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

<details>
<summary>01｜内存池分配与释放分别计时</summary>

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

</details>

<a id="source-02"></a>

<details>
<summary>02｜Logger 输入相同，并验证真正排空的结果</summary>

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

</details>

<a id="source-03"></a>

<details>
<summary>03｜订单簿基准消费并比较全部输出</summary>

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

</details>

<a id="source-04"></a>

<details>
<summary>04｜新增开环生成器，保留计划时间和短写进度</summary>

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

</details>

<a id="source-05"></a>

<details>
<summary>05｜撤单回报的成交数量按“不适用”检查</summary>

首次冒烟误把撤单 exec_qty 的预期设为 0；真实协议返回 Qty_INVALID，即 0xFFFFFFFF。失败来自探针假设，而不是交易所协议需要改变。

**修改前**

初版脚本未提交，其完整旧源码没有保留在可核实的 Git 版本中；保留了失败回包和 RuntimeError。此处不根据记忆拼出旧代码，只展示可核实的修正后实现与失败证据。



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

</details>

<a id="source-06"></a>

<details>
<summary>06｜新增分析器，将迟到回报与窗口吞吐分开</summary>

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

</details>

<a id="source-07"></a>

<details>
<summary>07｜CPU 累计量和时间分母使用同一采样区间</summary>

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

</details>

<a id="source-08"></a>

<details>
<summary>08｜脚本入口使用统一、可核查的测量流程</summary>

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

</details>
