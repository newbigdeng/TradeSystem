# 03 源码对照

[返回主报告](03_performance_tuning.md)。点击具体问题展开代码。

片段取自实际 Git 版本或已核实的工作区，标注文件、版本及行号。片段省略上下文；完整实现见对应版本链接。新增功能注明原来没有该实现，未保存的旧辅助源码注明缺失。

候选 CMake、计时宏、快照接口和实验源码尚未提交。片段附工作区摘要，公开报告对应已保存的测量版本。

| 对照 | 内容 |
|---|---|
| 01 | [新增性能日志编译配置，保持默认全量](#source-01) |
| 02 | [关闭模式不读取时钟，也不求值 Logger 参数](#source-02) |
| 03 | [抽样计数限定到各线程及各调用位置](#source-03) |
| 04 | [快照公开方法明确返回 void](#source-04) |
| 05 | [生成器校验的回包器改成独立进程](#source-05) |
| 06 | [空计时循环使用自己的耗时分母](#source-06) |

<a id="source-01"></a>

<details>
<summary>01｜新增性能日志编译配置，保持默认全量</summary>

原构建没有性能宏模式配置；性能记录在空转回调中仍反复产生。候选通过构建配置控制这些可选诊断记录。

**修改前**

[`CMakeLists.txt`，`f91bd36`，第 5–8 行](https://github.com/newbigdeng/TradeSystem/blob/f91bd36b50d3d98e77a511f695e3660833f2ffda/CMakeLists.txt#L5)

```cmake
add_library(trade_build_options INTERFACE)
target_compile_features(trade_build_options INTERFACE cxx_std_20)
target_compile_options(trade_build_options INTERFACE -Wall -Wextra -Werror -Wpedantic)
set(TRADE_SANITIZER "" CACHE STRING "address or thread; empty for ordinary builds")
```


**修改后**

`CMakeLists.txt`；工作区未提交，源码 SHA-256：`f3470987a3d60fb36909941941c6f729eb0edcbbf7dc22d40e2f93abeac9b710`

```cmake
add_library(trade_build_options INTERFACE)
target_compile_features(trade_build_options INTERFACE cxx_std_20)
target_compile_options(trade_build_options INTERFACE -Wall -Wextra -Werror -Wpedantic)
set(TRADE_PERF_TRACE_EVERY "1" CACHE STRING "Diagnostic timing records: 0 off, 1 full, N one in N calls per thread/site")
if(NOT TRADE_PERF_TRACE_EVERY MATCHES "^(0|[1-9][0-9]*)$" OR TRADE_PERF_TRACE_EVERY GREATER 1000000)
  message(FATAL_ERROR "TRADE_PERF_TRACE_EVERY must be an integer from 0 to 1000000")
endif()
target_compile_definitions(trade_build_options INTERFACE TRADE_PERF_TRACE_EVERY=${TRADE_PERF_TRACE_EVERY})
set(TRADE_SANITIZER "" CACHE STRING "address or thread; empty for ordinary builds")
```


**为什么这样改，以及如何验证：** 默认 1 保留原行为，64 为按调用次数抽样，0 关闭；CMake 拒绝非法或大于 1,000,000 的值。没有改变普通业务/错误日志和关键审计。对应五轮配对结果见 [paired_summary.json](evidence/paired_summary.json)，所有五轮尾延迟门槛仍未通过。

</details>

<a id="source-02"></a>

<details>
<summary>02｜关闭模式不读取时钟，也不求值 Logger 参数</summary>

旧 START/END/TTT 每次都会读时钟并输出诊断；关闭模式必须连参数求值都取消，而不只是丢掉输出。

**修改前**

[`common/perf_utils.h`，`f91bd36`，第 13–27 行](https://github.com/newbigdeng/TradeSystem/blob/f91bd36b50d3d98e77a511f695e3660833f2ffda/common/perf_utils.h#L13)

```cpp
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// End latency measurement using rdtsc(). Expects a variable called TAG to already exist in the local scope.
#define END_MEASURE(TAG, LOGGER)                                                              \
      do {                                                                                    \
        const auto end = Common::rdtsc();                                                     \
        LOGGER.log("% RDTSC "#TAG" %\n", Common::getCurrentTimeStr(), (end - TAG)); \
      } while(false)

/// Log a current timestamp at the time this macro is invoked.
#define TTT_MEASURE(TAG, LOGGER)                                                              \
      do {                                                                                    \
        const auto TAG = Common::getCurrentNanos();                                           \
        LOGGER.log("% TTT "#TAG" %\n", Common::getCurrentTimeStr(), TAG);           \
      } while(false)
```


**修改后**

`common/perf_utils.h`；工作区未提交，源码 SHA-256：`dc51ea68216cb1d3bef87c3ecd84deeb230bf2dbad17e88a3de9b40294ca818d`

```cpp
#if TRADE_PERF_TRACE_EVERY == 0
// Disabled instrumentation must not read a clock or evaluate logger arguments.
#define START_MEASURE(TAG) do {} while(false)
#define END_MEASURE(TAG, LOGGER) do {} while(false)
#define TTT_MEASURE(TAG, LOGGER) do {} while(false)
```


**为什么这样改，以及如何验证：** 三个宏均为空 do/while；使用含副作用的假 Logger 参数验证其求值次数为零。模式 1 的宏体仍保留，TSC 注释改为未标定诊断 tick。三模式专项计数与 sanitizer 结果见 [final_validation.json](evidence/final_validation.json)；关闭日志也提高了忙轮询 CPU 占用，原结果不改写。

</details>

<a id="source-03"></a>

<details>
<summary>03｜抽样计数限定到各线程及各调用位置</summary>

父版本只有全量宏，没有抽样分支。新增线程局部计数，避免多个线程对共享采样计数器产生竞争。

**修改前**

旧宏的完整片段见对照 02；新增分支在父版本不存在。



**修改后**

`common/perf_utils.h`；工作区未提交，源码 SHA-256：`dc51ea68216cb1d3bef87c3ecd84deeb230bf2dbad17e88a3de9b40294ca818d`

```cpp
// A deterministic counter belongs to each call site and each owning thread.
// This selects calls, not a representative statistical sample of slow orders.
#define START_MEASURE(TAG) \
  static thread_local uint64_t TAG##_sample_count = 0; \
  const auto TAG = (++TAG##_sample_count % TRADE_PERF_TRACE_EVERY == 0) ? Common::rdtsc() : uint64_t{0}
#define END_MEASURE(TAG, LOGGER) \
  do { \
    if (TAG) { \
      const auto end = Common::rdtsc(); \
      LOGGER.log("% RDTSC "#TAG" %\n", Common::getCurrentTimeStr(), end - TAG); \
    } \
  } while(false)
#define TTT_MEASURE(TAG, LOGGER) \
  do { \
    static thread_local uint64_t TAG##_sample_count = 0; \
    if (++TAG##_sample_count % TRADE_PERF_TRACE_EVERY == 0) { \
      const auto TAG = Common::getCurrentNanos(); \
      LOGGER.log("% TTT "#TAG" %\n", Common::getCurrentTimeStr(), TAG); \
    } \
  } while(false)
#endif
```


**为什么这样改，以及如何验证：** START 仅在选中时读取 TSC，END 仅在 TAG 非零时输出；TTT 使用同样的确定性计数。两线程各 1,024 次专项检查：全量 4,096 条，1/64 为 64 条，关闭为 0。抽样日志不能直接用来推算全体订单 P99；五轮中的变慢结果仍保留。

</details>

<a id="source-04"></a>

<details>
<summary>04｜快照公开方法明确返回 void</summary>

头文件的 auto 仅有声明；外部编译单元看不到 .cpp 中定义，无法推导返回类型，因而报 use before deduction of auto。

**修改前**

[`exchange/market_data/snapshot_synthesizer.h`，`f91bd36`，第 30–33 行](https://github.com/newbigdeng/TradeSystem/blob/f91bd36b50d3d98e77a511f695e3660833f2ffda/exchange/market_data/snapshot_synthesizer.h#L30)

```cpp
    auto addToSnapshot(const MDPMarketUpdate *market_update);

    /// Publish a full snapshot cycle on the snapshot multicast stream.
    auto publishSnapshot();
```

[`exchange/market_data/snapshot_synthesizer.cpp`，`f91bd36`，第 31–31 行](https://github.com/newbigdeng/TradeSystem/blob/f91bd36b50d3d98e77a511f695e3660833f2ffda/exchange/market_data/snapshot_synthesizer.cpp#L31)

```cpp
  auto SnapshotSynthesizer::addToSnapshot(const MDPMarketUpdate *market_update) {
```

[`exchange/market_data/snapshot_synthesizer.cpp`，`f91bd36`，第 78–78 行](https://github.com/newbigdeng/TradeSystem/blob/f91bd36b50d3d98e77a511f695e3660833f2ffda/exchange/market_data/snapshot_synthesizer.cpp#L78)

```cpp
  auto SnapshotSynthesizer::publishSnapshot() {
```


**修改后**

`exchange/market_data/snapshot_synthesizer.h`；工作区未提交，源码 SHA-256：`6fca62a2da270a66808b1ef44863d1a1e65d656f24d118ffdc631efd1525f4d7`

```cpp
    void addToSnapshot(const MDPMarketUpdate *market_update);

    /// Publish a full snapshot cycle on the snapshot multicast stream.
    void publishSnapshot();
```

`exchange/market_data/snapshot_synthesizer.cpp`；工作区未提交，源码 SHA-256：`4f2d7e58ec2cc8dd6e82ced47ad3b616c4cde5f876988c8a04851f570a6a9bd8`

```cpp
  void SnapshotSynthesizer::addToSnapshot(const MDPMarketUpdate *market_update) {
```

`exchange/market_data/snapshot_synthesizer.cpp`；工作区未提交，源码 SHA-256：`4f2d7e58ec2cc8dd6e82ced47ad3b616c4cde5f876988c8a04851f570a6a9bd8`

```cpp
  void SnapshotSynthesizer::publishSnapshot() {
```


**为什么这样改，以及如何验证：** 声明与定义同时改成 void，移除实验中直接包含 .cpp 的绕过后正常链接。独立编译从失败变为通过，0/1,000/10,000 单快照再验收；目标文件及已测二进制字节相同。见 [api_machine_code_check.json](evidence/api_machine_code_check.json)、[binary_continuity.json](evidence/binary_continuity.json)。这是接口修复，没有运行时提速收益。

</details>

<a id="source-05"></a>

<details>
<summary>05｜生成器校验的回包器改成独立进程</summary>

初版辅助校验在同一 Python 进程内用两个线程，发单器和理想回包器共享 GIL。这里修正实验隔离，实际交易所对照原本就是 Python+C++ 两个进程。

**修改前**

两份辅助脚本都未提交；“修改前”取初版 generator_probe.py，“修改后”取新 generator_process_probe.py，不把前者误称 Git 历史版本。

`record/03_performance_tuning/experiments/generator_probe.py`；工作区未提交，源码 SHA-256：`9cb6b9eb36c73cec9a62a2afae60195f9f51da7e6daffaa509db9043c02e6cde`

```python
            stop=threading.Event();worker=threading.Thread(target=echo,args=(servers,stop));worker.start()
            try:rows,start,end,counts=drive(clients,requests(rate*10,4,8,20260928),'open',rate,10)
            finally:stop.set();worker.join()
```


**修改后**

`record/03_performance_tuning/experiments/generator_process_probe.py`；工作区未提交，源码 SHA-256：`9e51a811b6bf0690fd3a08b130f3de07af02bcbdc4f89aa155a8b981a3f88771`

```python
            context=multiprocessing.get_context('fork');stop=context.Event();worker=context.Process(target=echo,args=(servers,stop));worker.start()
            source=requests(rate*10,4,8,20260928)
            payload=json.dumps(source,sort_keys=True,separators=(',',':'))+'\n'
            try:rows,start,end,counts=drive(clients,source,'open',rate,10)
            finally:stop.set();worker.join()
```


**为什么这样改，以及如何验证：** 保持相同 drive、socketpair、输入种子和 CPU 放置，重新测十轮并保留初版。独立进程的发送迟到没有变好，不能包装为性能收益。见 [generator_methodology.json](evidence/generator_methodology.json) 与正文两版对照。

</details>

<a id="source-06"></a>

<details>
<summary>06｜空计时循环使用自己的耗时分母</summary>

初版 clock_probe 的空计时 throughput 错用了开启事件采集循环的总耗时。已保存初版 CSV 与验证输出，但没有冻结初版辅助源码文件。

**修改前**

初版具体源码未独立保留，不能凭描述重建为“原始代码”。旧错误口径及测量输出可核实；下面展示当前真实修正后代码。



**修改后**

`record/03_performance_tuning/experiments/clock_probe.cpp`；工作区未提交，源码 SHA-256：`bd0fb68ff50b67cdb18524e5136a990a4437003b0e0fe0b6be531ded620c6287`

```cpp
  const auto empty_start=Measure::now();
  for(auto& ns:disabled){const auto a=Measure::now();ns=Measure::now()-a;}
  const auto empty_elapsed=Measure::now()-empty_start;
```

`record/03_performance_tuning/experiments/clock_probe.cpp`；工作区未提交，源码 SHA-256：`bd0fb68ff50b67cdb18524e5136a990a4437003b0e0fe0b6be531ded620c6287`

```cpp
  const auto window=Measure::now();
  for(auto& ns:enabled){const auto a=Measure::now();StageTrace::event(key,0);ns=Measure::now()-a;}
  const auto elapsed=Measure::now()-window;
  Measure::output("stage_record_call",enabled,elapsed,0);Measure::output("stage_empty_clock_pair",disabled,empty_elapsed,0);
```


**为什么这样改，以及如何验证：** 空循环与开启循环各自计整体时间，输出使用各自分母；每调用时长样本不机械扣除空时钟。旧吞吐字段弃用，样本保留，见 [clock_methodology.json](evidence/clock_methodology.json)、[clock_probe_v1.txt](evidence/validation/clock_probe_v1.txt) / [clock_probe_v2.txt](evidence/validation/clock_probe_v2.txt)。正式网络测量不受该辅助错误影响。

</details>
