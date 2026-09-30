# 01｜可靠性改造：从故障复现到业务验收与性能复测

本次对 TradeSystem 的消息传递、订单处理、行情恢复、线程停机及交易审计进行了可靠性改造。代码在 Ubuntu 虚拟机的 `/home/zjh/XiangMu/TradeSystem-rebuild` 中编译和运行，报告、测试与测量结果统一放在本目录。

这次解决的主要问题是：队列满时覆盖旧消息，TCP 部分发送丢尾部，内存池少用一个槽，相同时间戳改变订单顺序，价格碰撞混淆档位，部分发单路径绕过风控，以及不完整行情快照被当作恢复成功。随后补齐了线程停机、逐品种特征、并发日志时间和关键交易审计。

采用的顺序是 **先测量并保存失败证据 → 完成一项改造 → 验证问题已修复 → 再测量 → 进入下一项**。这是可靠性基线 B0 到修复版 B1 的比较。B0 在某些故障输入上有错误，不能把下面的耗时变化写成“已证明整个交易系统提速了多少”；正式优化还需要在两个都满足全部不变量的版本之间做 B1/B2 对照。

## 一、测试时发现有问题

### 1. 测试环境与比较方法

| 项目 | 本次使用值 |
|---|---|
| 操作系统 | Ubuntu 26.04.1 LTS，内核 `7.0.0-34-generic` |
| 工具链 | GCC 15.2.0，CMake 4.2.3，Ninja，C++20 |
| 虚拟机资源 | 8 个 vCPU，约 7.2 GiB 可见内存，4 GiB swap |
| 网络 | 同机回环 TCP；增量和快照组播均明确使用 `lo`；UFW 保持开启 |
| 普通基准 | `-O3 -DNDEBUG`，绑 CPU 0，预热 1 次，正式测量 5 次，报告中位数 |
| 端到端基准 | 进程绑 CPU 0–3；每轮 200 次新单及 200 次撤单，逐笔等待并验证回报；预热 1 轮、正式 5 轮 |
| 动态检查 | Debug、ASan+UBSan、TSan 分目录构建；Sanitizer 不用于性能比较 |

WT 和 WTPY 仍遵循容器运行方式；本报告只改造 TradeSystem，没有将 WT/WTPY 移到虚拟机宿主环境执行。

计时区间内保留业务校验或校验和。队列基准比较 200 万次有效 push/pop，TCP 基准比较完整的 16 MiB 字节流；旧版的溢出或丢包输入另做故障复现，不用“少处理消息”换取更低耗时。部件基准不计构造和析构；线程基准则专门测量创建加 join。

### 2. 改造前的实际失败

| 测试对象 | 预期 | 旧代码的实际结果 |
|---|---|---|
| 容量为 2 的队列再写第 3 条 | 拒绝第 3 条，原有两条保持顺序 | 接受写入并覆盖旧消息 |
| TCP 小发送缓冲，暂停接收 | 保留尚未发出的字节，恢复后字节流完全一致 | 部分发送后清空用户缓冲，丢失尾部 |
| 容量为 2 的内存池 | 前两次成功，第 3 次返回无空间 | 第 2 次有效分配就退出 |
| 33 条相同接收时间戳的订单 | 保持原到达顺序 | 排序改变原顺序 |
| 同侧价格 100 和 356 | 是两个独立档位，撤掉 100 不影响 356 | 两种撮合簿实现均发生碰撞 |
| 撤单被拒绝 | 离开 `PENDING_CANCEL`，按剩余数量恢复状态 | 卡在等待撤单状态 |
| RANDOM 发送超限订单 | 统一风控拒绝 | 直接进入发单队列 |
| 两个品种的特征 | 各自保存值 | 第二个品种覆盖第一个 |
| 快照只有 START/END，缺少 CLEAR | 保持恢复状态 | 被判定为恢复完成 |
| 两个线程同时记录发单日志 | 时间缓冲互不干扰 | TSan 报告时间格式化与共享字符串的数据竞争，退出码 66 |

原始证据分别保存在 [baseline_results.json](baseline_results.json)、[book_baseline_failures.json](book_baseline_failures.json)、[control_baseline_failures.json](control_baseline_failures.json)、[protocol_baseline_failure.json](protocol_baseline_failure.json)、[recovery_baseline_failure.json](recovery_baseline_failure.json) 和 [concurrent_baseline_failure.json](concurrent_baseline_failure.json)。最后一项是在前面改造完成后追加检查发现的，随后也按“先测再改再测”的顺序处理。

## 二、寻找问题

### 1. 基础部件的容量和生命周期不完整

`common/lf_queue.h` 的旧写入接口总是给出一个槽，没有队列满的失败结果。仅更改队列内部还不够，所有生产者必须处理失败，否则业务仍会以为消息已经交给下游。普通 Logger 按零碎值写队列，多线程会交错，容量不足时还可能丢掉半条日志。

`common/tcp_socket.h` 没有保留 `[begin,end)` 的待发送范围，把一次 `send()` 当成完整发送。非阻塞 socket 的部分写入和 `EAGAIN` 都是正常背压现象，不能直接清空余量。订单序号与请求必须整体进入缓冲，失败时不能推进序号。接收侧也必须区分无数据、EOF 和连接错误。

内存池先推进空闲索引再判断容量，错误地把最后一次合法分配当成耗尽；对象回收也缺少明确析构。线程启动函数额外固定等待一秒，多个 stop/destructor 用等待代替 join，无法证明工作线程已经停止。

### 2. 订单和行情的身份不够明确

两种撮合簿都把 `price % 256` 当作价格档位身份。100 和 356 得到相同键，造成撤单与跨价成交错误。原 `(ClientId,OrderId)` 固定指针表每本簿约 2 GiB，八本簿占用大量虚拟地址空间。

原订单传输直接复制 packed C++ 结构体，没有协议版本、明确字节序和会话代次。TCP 断线只能说明连接断了，不能说明此前的订单没到交易所。重发同一经济意图可能形成重复订单。因此需要独立区分传输序号、会话身份、经济订单 ID 和成交回报 ID。

风险检查只看已经成交的持仓，未计入已发送但未成交的订单。比如最大仓位为 10，连续两笔买单各 6，即使当前仓位是 0，也可能最终到达 12。撤单请求发出后，原订单仍可能成交，预占额度不能立即释放。

### 3. “收到快照”不足以证明恢复完成

只看到 START/END 和局部连续序号，不能证明所有品种都被清空、快照属于同一周期，或重建后的簿与中心簿相同。恢复期间的增量必须从同一快照水位之后连续衔接。下游队列满时，若只发布半个恢复批次，客户端会暴露一个似乎可用的残缺簿。

最后的并发复现还定位到 `common/time_utils.h`：`ctime()` 使用共享内部缓冲，TradeEngine 的多个入口又写同一个 `time_str_`。Logger 自身加锁并不能保护调用 Logger 之前的这些操作。

## 三、解决问题

### 1. 有界队列和明确的满队列处理

队列改为 SPSC 的 `try_push/try_pop`；满时返回 `false` 且原内容不变，空时也返回 `false`。索引按 `2N` 回绕，支持容量 1、2、3 等任意正容量。补充 `full_count`、最高占用量及整个批次一次发布的接口。

所有旧写入接口调用已迁移，具体行为如下：

| 链路 | 队列满时的行为 |
|---|---|
| 客户端发单 | 返回失败，回滚本次风险预占，不伪造已发状态 |
| FIFO → 撮合 | 保留当前订单，等待下游腾出空间 |
| 网关接收回报 | 保留尚未入队的完整帧，下轮继续处理 |
| 撮合回报、关键行情输出 | 显式故障封闭；回报先写关键审计，禁止无声覆盖 |
| 行情 → 客户端簿 | 标为不可信，停止依赖该簿的新单并重新恢复 |
| 普通诊断 Logger | 整条接受或整条丢弃，明确累计丢弃数；多个写入者串行化 |

这里的 SPSC 约束没有变成 MPSC。TradeEngine 多入口通过互斥锁串行化，Logger 的多写入者也在入队前串行化。

### 2. TCP、内存池和订单顺序

TCP 保留未发送范围，按实际发送字节数推进。`EINTR` 重试，`EAGAIN` 留到下一轮，EOF/错误进入可观察的连接状态。每轮发送有系统调用预算。整帧容量足够才复制，只有接受成功才出队并推进传输序号。接收残片使用 `memmove` 保留；即使没有新的 EPOLLIN，也继续尝试处理因下游满而留下的帧。

修正非阻塞 connect 的 `0/EINPROGRESS/SO_ERROR` 判断，释放地址查询资源；socket、epoll 和接入连接有明确所有者和析构。连接总量设上界，TCP 默认用户缓冲从 64 MiB 改为 1 MiB，通过已有背压语义处理容量不足。

内存池用显式构造、析构和空闲槽表，容量 N 可完整使用，第 N+1 次返回空；非法归还和重复归还有断言。普通与 Opt 版本共享这套已验证实现，不能把旧 benchmark 名称当作两套独立优化实现的比较。

FIFO 使用稳定排序。不同时间戳按时间排序，相同时间戳保持收集顺序。

### 3. 撮合簿、协议、订单状态及风控

价格索引使用完整 `(side,price)`，两种撮合簿都修正；客户订单表改为按客户的稀疏映射。订单加入前检查 ID、品种、方向、价格、数量、重复经济 ID 和容量。客户端行情簿也校验 ADD/MODIFY/CANCEL，初始化指针，CLEAR 释放状态并重置 BBO。

订单协议改为固定宽度、大端字节序，包含魔数、版本、消息种类、会话代次和序号。请求帧为 50 字节，回报为 79 字节，行情帧为 70 字节。接入 socket 与客户身份绑定，身份、序号、会话或字段错误有明确拒绝原因；数组索引前完成校验。

增加成交回报 ID 和交易所持仓。重复成交回报不再重复改变 PositionKeeper；成交数量加剩余数量必须等于原预占数量，数量矛盾或对账持仓不一致就停止新单。撤单拒绝恢复订单状态，迟到的 ACCEPTED 不复活已结束订单。重复经济 ID 的拒绝不贸然释放原订单预占；撤单拒绝的剩余量与本地预占不一致也进入待对账状态。

包括 RANDOM 在内的所有新单走最终风控门。风险预占覆盖当前持仓加全部未完成买单/卖单的最坏情形；只有成交、明确撤单或新单拒绝后才相应释放。行情不可信、订单会话未知或账户未对账时，不允许新单。

增加 QUERY：可以查询已知订单剩余量以及交易所持仓；撤掉的订单查询返回剩余量 0。断线账户使用旧会话代次重新连接时只允许只读查询，新经济订单仍被阻止，没有自动重发订单。

### 4. 有证据的行情恢复

提取 [MarketRecovery](../../trading/market_data/market_recovery.h)，可直接注入消息。每个快照携带周期、水位和确定性簿哈希，要求 START、全部 8 个品种的 CLEAR、完整数据及 END 来自同一个周期；之后接续连续增量。重复冲突、缺口、错误哈希或缓存超过上界均保持恢复状态。

重建过程先在内部完成，再把完整批次一次放进下游队列，容量不足时不发布半批。批末的内部 `RECOVERY_COMMIT` 让 TradeEngine 在相同水位再次计算本地簿哈希，并核对恢复代次；通过后才将行情标为可信。缓存有硬上限，UDP 截断和错误帧也会触发恢复。

### 5. 停机、数值边界与关键审计

运行标记改为原子变量，线程由对象持有并 join；去掉固定一秒/五秒的等待。信号处理只设置停止标记。交易所依次停止接单、排空 FIFO 和撮合、排空行情并发布最终快照、排空回报；客户端停新单，尝试撤销剩余风险预占，再排空网关和交易线程。关键输出在期限内无法排空时明确失败，进入待对账状态。

特征按品种保存，非法盘口重置为 NaN，策略通过 `std::isfinite` 检查；零深度不计算，使用更宽的持仓/数量和明确的浮点运算处理数值边界。时间格式化改用 `ctime_r` 和调用内的缓冲，业务日志使用返回值，不再共同修改成员字符串。排空期限使用单调时钟。

普通 Logger 是可丢的诊断日志。新增 [CriticalJournal](../../common/critical_journal.h) 独立保存关键交易事件，每条写入后 `fsync`：接收请求记录 RECEIVED，进入撮合记录 APPLY，发布回报前记录 RESPONSE；账户退出保存会话、持仓、成交量和未解决订单。创建时同步父目录，并拒绝覆盖已有审计文件。写入失败或同步失败会明确退出。

**APPLY 表示进入处理，不是完整事务提交标记。** [reconcile.py](reconcile.py) 是只读对账工具：检查请求与处理记录、处理请求与确认数量、成交剩余量守恒、买卖成交量平衡、回报 ID 和持仓，再比对账户检查点。它不自动重放订单，不自动恢复撮合进程，也不自动批准账户重新交易。

## 四、重新测试：问题修复了没有，性能变化多少

### 1. 每项改造的测量结果

下表使用各阶段原始 JSON 的五轮中位数。下降比例为 `(改造前−改造后)/改造前`；“增加”明确表示更慢。单次操作的定义各不相同，不能把这些比例相加。

数值及比例汇总另见 [performance_summary.json](performance_summary.json)，每个原始文件仍保留五轮样本和校验和。

| 改造项及计量单位 | 改造前 | 改造后 | 本轮变化 | 结果与证据 |
|---|---:|---:|---:|---|
| 队列：一次 push+pop | 237.598 ns | 16.4689 ns | 耗时下降 93.07% | [前](perf_queue_before.json) / [后](perf_queue_after.json)；满队列不覆盖，容量 3 下三轮百万序号正确 |
| TCP：一块 512 字节发送及接收 | 20,620.2 ns | 6,785.19 ns | 耗时下降 67.09% | [前](perf_tcp_before.json) / [后](perf_tcp_after.json)；3 MiB 背压流逐字节一致 |
| 内存池：一次分配+归还 | 323.932 ns | 90.7674 ns | 耗时下降 71.98% | [前](perf_pool_before.json) / [后](perf_pool_after.json)；第 N 个槽可用，析构计数正确 |
| FIFO：排序一批 64 条 | 23,736.4 ns | 22,911.5 ns | 耗时下降 3.48% | [前](perf_fifo_before.json) / [后](perf_fifo_after.json)；幅度较小，不认定显著提速，等时间戳顺序已修复 |
| 撮合簿：一次 NEW 或 CANCEL | 75,277 ns | 75,776.1 ns | 耗时增加 0.66% | [前](perf_book_before.json) / [后](perf_book_after.json)；性能基本持平，碰撞与状态正确性已修复 |
| 风控：发单+确认+撤单释放一轮 | 95,140.9 ns | 95,915.3 ns | 耗时增加 0.81% | [前](perf_admission_benchmark_before.json) / [后](perf_admission_benchmark_after.json)；统一门和待成交预占生效 |
| 协议：请求编码+解码 | 32.9082 ns | 89.961 ns | 耗时增加 173.37% | [前](perf_protocol_before.json) / [后](perf_protocol_after.json)；旧结构复制与新版协议的功能、帧长不同，这是可靠性成本 |
| 恢复：含 8 CLEAR+64 ADD 的一轮 | 1,004,680 ns | 58,870.5 ns | 耗时下降 94.14% | [前](perf_recovery_before.json) / [后](perf_recovery_after.json)；每轮有效输出 72 条，减少了逐消息诊断开销，并增加了完整性与哈希校验 |
| 特征：一次有效品种更新 | 14,247.9 ns | 3,270.96 ns | 耗时下降 77.04% | [前](perf_feature_benchmark_before.json) / [后](perf_feature_benchmark_after.json)；含日志路径变化，不等于纯算术加速 |
| 线程：创建+join | 1,000,561,578 ns | 491,661 ns | 耗时下降 99.95% | [前](perf_lifecycle_before.json) / [后](perf_lifecycle_after.json)；主要是移除固定一秒等待，不代表交易延迟缩短一秒 |
| 时间：一次格式化 | 5,701.05 ns | 1,734.33 ns | 耗时下降 69.58% | [前](perf_clock_before.json) / [后](perf_clock_after.json)；并发竞争有 TSan 的红灯证据，修复后重新检查 |

队列、TCP、池及排序的构造与析构均在计时区间外。恢复对比是进程内状态机和输出校验，**不是从发现真实网络丢包到恢复交易的墙钟延迟**。虚拟机调度、频率和日志线程会带来波动，本轮未给出统计置信区间。

索引内存单独比较：[book_probe.cpp](book_probe.cpp) 中 8 本交易所簿加 1 本额外测试簿，单本 `sizeof` 从 2,147,485,952 降到 29,040 字节；进程 VmSize 从 19,844,232 kB 降到 970,716 kB。RSS 则约从 820,096 kB 到 820,760 kB，基本不变。旧巨型指针表并未全部触页，新结构仍有对象池和堆存储，因此不能声称释放了约 18 GiB 物理内存。

### 2. 审计和停机改造的端到端成本

在完成审计/停机改造之前先运行相同有效订单流，然后重新测量。每轮回报均验证版本、会话、传输序号、客户、订单 ID 和回报类型，400 条输出的校验和均为 40,200。

| 指标：五轮对应分位数的中位数 | 改造前 | 改造后 | 变化 |
|---|---:|---:|---:|
| 订单确认 P50 | 1.415014 ms | 1.706806 ms | 增加 20.62% |
| 订单确认 P99 | 4.608310 ms | 5.033436 ms | 增加 9.23% |
| 收到 SIGTERM 后的进程退出码 | `-15`，信号直接结束 | `0`，排空后退出 | 已修复正常停机路径 |

原始数据：[perf_e2e_before.json](perf_e2e_before.json)、[perf_e2e_after.json](perf_e2e_after.json)，驱动：[e2e_benchmark.py](e2e_benchmark.py)。这是审计、生命周期和数值改造这一阶段的综合变化，不能把全部增加的耗时严格归因于某一个 `fsync`。这轮结果有成本，没有将它包装为系统整体提速。

时间竞争修复之后另做最终端到端测量，保留为独立的 [perf_e2e_final.json](perf_e2e_final.json)，不覆盖本阶段的 after 数据。该轮没有与编译或 Sanitizer 同时运行，审计落盘仍开启。

| 最终版本 `9572484` | 最终值 | 对比审计/停机改造前的中间版本 `1c0af86` | 对比刚完成审计/停机改造的 after 阶段 |
|---|---:|---:|---:|
| 订单确认 P50 | 0.300444 ms | 下降 78.77% | 下降 82.40% |
| 订单确认 P99 | 1.904154 ms | 下降 58.68% | 下降 62.17% |

五个正式轮次均得到 400 条正确回报，校验和 40,200，正常排空退出码均为 0。最后这一步主要改变了时间格式化和并发日志的调用方式；它的收益是在这台虚拟机、本组负载中测得的。这里的端到端基准起点已经包含前面的队列、TCP、簿、风控和恢复修复，**不是最初基线版本 `ff20fd7`**。各阶段的收益和成本因此分开保留。

### 3. 回归与动态检查

最终检查结果见 [validation_profiles.json](validation_profiles.json)。Release、Debug、ASan+UBSan、TSan 使用各自构建目录，编译命令随结果保存，避免 CMake 覆盖 flags 后实际没有开启检查。

| 最终检查 | 实际结果 |
|---|---|
| Release | 16/16 通过 |
| 未优化 Debug | 16/16 通过 |
| ASan+UBSan，`-g -O1` | 16/16 通过 |
| TSan，`-g -O1`，独立构建 | 16/16 通过 |
| 真实进程联调：Release、ASan/UBSan、TSan | 三轮均通过，无诊断告警，退出码均为 0 |
| 对账工具的故障测试 | 7/7 通过 |

首轮未优化的诊断构建中，两种簿的模型测试超过 180 秒；TSan 真实进程联调也因初始化超过驱动原先的 25 秒等待而退出。原始轮次保存在 [validation_profiles_first_attempt.json](validation_profiles_first_attempt.json) 和 [integration_tsan_first_attempt.json](integration_tsan_first_attempt.json)，没有删除。Sanitizer 改为带 `-g` 的 `-O1` 构建，联调增加诊断进程的初始化等待；仍运行完整 12,000 条模型输入。普通 Debug 保持未优化，性能测量保持 Release `-O3`，三者不混用。

回归测试包括：容量 1/2/3、三轮百万序号与四个日志写入线程；小发送缓冲和拆包/粘包/EOF；池容量与非平凡析构；等时间戳 FIFO；两种撮合簿各 12,000 条固定种子 `0x51A7` 输入，与独立 `map<Price,deque<Order>>` 模型逐事件比较回报、成交量、活动订单、BBO 和客户端簿；协议及风险状态；快照故障矩阵；多线程发单及关键审计。

恢复故障矩阵包含：缺 START/CLEAR/END、跨周期交错、重复/乱序、快照哈希错误、增量缺口后补齐、缓存溢出、输出队列满、超大批次与回绕。失败时保持恢复状态；正确周期到来后才交付完整批次。

### 4. 多客户端真实进程验收

[integration_test.py](integration_test.py) 使用真实 `exchange_main` 和两个 `trading_main RANDOM`，每个客户端最多尝试 400 笔新单，随机种子由客户 ID 决定；第二个客户端晚 1 秒启动。测试用快照周期 200 ms，正常默认周期仍为 60 秒。

结果保存在 [integration_result.json](integration_result.json)：客户端最终风控接受数与网关发送数、交易所持久化接收数相等；客户端接收回报数与审计中的该客户撮合回报数相等；两客户持仓及成交量与账户检查点一致；无未处理请求，无未解决预占，最终没有活动订单。晚启动客户端通过恢复哈希验证。所有请求/回报/行情队列排空，三个进程正常退出。

最终 Release 轮的计数：客户端 1 接受/发送/交易所接收均为 830，客户端 2 均为 864；两客户各收到 1,336 条撮合回报。连同查询测试账户的 7 个有效请求，审计记录 RECEIVED 和 APPLY 均为 1,701。共有 2,690 条 RESPONSE，其中 2,679 条有撮合回报 ID，另 11 条为接入层协议拒绝。两客户的新单风控拒绝各 6 次，不能将这些未放行尝试当作消息丢失。请求队列最高占用分别为 26 和 67，队列满计数均为 0。

同一驱动还验证 18 个有效或故障场景：身份错误、序号缺口、错误会话、零量、非法品种/价格/方向/类型/客户 ID、错误版本、重复经济 ID、活动/已撤/不存在订单查询、断线只读查询以及断线后新单被阻止。已有审计文件下再次启动被拒绝，文件字节完全不变。真实原始日志和 journal 留在 JSON 指向的虚拟机 `/tmp/trade-integration-*` 目录；摘要和可复现驱动进入仓库。

另外使用 ASan/UBSan 和 TSan 构建分别运行同样的真实进程故障驱动，诊断轮每客户最多尝试 100 笔新单、运行期限 12 秒。结果分别保留为 [integration_asan_result.json](integration_asan_result.json) 和 [integration_tsan_result.json](integration_tsan_result.json)，这些轮次用于检查正确性和诊断告警，不与 Release 性能值比较。

只读对账工具另有 [reconcile_test.py](reconcile_test.py) 的 7 项检查：正确记录、已接收但未处理、进入处理但缺确认、缺一侧成交、重复成交、持仓偏差与末条记录截断。

### 5. 验收边界

这轮证明了已列明输入下的正确性、故障检测、行情恢复和有序停机。**没有实现崩溃后自动重建整个交易所、账户自动恢复交易或自动重发未确认订单。** 遇到旧 journal、断线未知状态、输出排空失败或对账矛盾时，程序会阻止继续新单，要求查询和人工确认。这是本次定义并测试的故障封闭边界。

`fsync` 的成功是本机操作系统确认同步；本轮没有做宿主机断电测试，没有强制注入磁盘满、每一种 `EINTR` 或所有 TCP reset 时机，也不能据此宣称生产级 exactly-once。测试中的关键队列溢出会明确退出，但对应回报在退出前保留在审计中。

## 五、版本和复现方式

### 1. 本轮提交顺序

| 提交 | 内容 |
|---|---|
| `67e63fd` | 组播报告和 C++ 测试归入 `record/00_multicast_interface` |
| `c88c13b` | 保存改造前队列/TCP/池/FIFO 失败记录 |
| `5f70930` | 有界队列、完整诊断记录和调用方迁移 |
| `33e804a` | TCP 待发数据与完整帧背压 |
| `f8b1471` | 内存池容量和对象生命周期 |
| `cc98699` | 相同时间戳稳定 FIFO |
| `cd36235` | 完整价格键、稀疏订单索引和模型比较 |
| `e107084` | 协议、订单状态、最后风控门与风险预占 |
| `1c0af86` | 快照周期、水位、簿哈希及原子恢复批次 |
| `701007c` | 线程停机、逐品种特征、关键审计及重启保护 |
| `9572484` | 时间格式化竞争修复，四种配置和真实进程诊断验收，补齐测量与对账工具 |

测量 JSON 的 `commit` 是采样时的 HEAD。部分 after 采样发生在本项改动完成、提交之前，所以与 before 的 HEAD 相同；不是在同一份源码上重复测量。上表给出对应实现提交，失败输入、有效负载和兼容宏保存在探针中；动态验收另保存逐文件源码 SHA-256。后续采样脚本同时记录工作区是否有修改，避免混淆测量时的源码状态。

### 2. 在当前修复版复测

在虚拟机项目根目录执行：

```bash
cmake -S . -B build/reliability-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/reliability-release -j4
ctest --test-dir build/reliability-release --output-on-failure
python3 record/01_reliability/validate_profiles.py
python3 record/01_reliability/integration_test.py
python3 record/01_reliability/integration_test.py asan
python3 record/01_reliability/integration_test.py tsan
python3 record/01_reliability/reconcile_test.py
```

诊断构建与业务测试结束后，单独复测性能，避免与编译或 Sanitizer 并发争用 CPU：

```bash
python3 record/01_reliability/measure.py queue repeat
python3 record/01_reliability/measure.py tcp repeat
python3 record/01_reliability/measure.py pool repeat
python3 record/01_reliability/measure.py fifo repeat
python3 record/01_reliability/measure_components.py book repeat
python3 record/01_reliability/measure_components.py protocol repeat
python3 record/01_reliability/measure_components.py recovery repeat
python3 record/01_reliability/measure_control.py repeat admission_benchmark feature_benchmark lifecycle
python3 record/01_reliability/measure_clock.py repeat
python3 record/01_reliability/e2e_benchmark.py repeat
```

使用 `repeat` 等新阶段名，生成新 JSON，不覆盖这次的 before/after 证据。端到端脚本发现 12345 已占用会退出，不会终止已有用户进程；各轮使用新建临时目录，以免覆盖历史账户。

旧版故障和基准必须在对应历史源码上执行。`TRADE_BASELINE`、`TRADE_CONTROL_BASELINE`、`TRADE_FEATURE_BASELINE`、`TRADE_PROTOCOL_BASELINE` 用于历史 API 兼容；[recovery_baseline.cpp](recovery_baseline.cpp) 只能配合 `e107084` 前后记录所指的旧 consumer 实现，不能放入当前 CTest。历史 book/recovery 探针用 `_Exit` 避开旧析构问题，它们没有被当作所有权或 Sanitizer 验收证据。

### 3. 查看运行账户的对账结果

```bash
python3 record/01_reliability/reconcile.py \
  /实际运行目录/exchange_orders.journal \
  /客户端1运行目录/trading_account_1.journal \
  /客户端2运行目录/trading_account_2.journal \
  --output /保存位置/reconciliation.json
```

一致返回 0；请求缺处理、缺确认、成交或持仓不符返回非零。文件不完整或格式未知也报错。看到“记录一致”仍不等于已批准自动重启：先确认未解决订单和现场账户状态，再决定归档旧模拟并开启全新一轮。启动程序不会帮用户删除或覆盖历史文件。
