# 01｜可靠性改造：从故障复现到业务验收与性能复测

补充说明（2026-10-03）：本报告新增[逐项源码修改前后对照](#source-comparison)，原失败记录、测试结果和性能数字保持原样。

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

<a id="source-comparison"></a>

## 源码修改前后对照

以下新增代码块直接摘自对应提交或本次核实的工作区，保留真实代码，仅将换行统一为 LF。每个块标注文件、版本和源文件行号；它们是关键片段，不是可独立编译的完整文件。历史“修改后”指该项修复提交时的实现，后续改动另列。新增功能明确说明原来没有相同实现；缺失的未提交旧源码不根据记忆重建。完整改动可通过所列历史版本和提交链接检查。

| 对照 | 内容 |
|---|---|
| 01 | [队列满时拒绝写入，保留已有消息](#source-01) |
| 02 | [生产者处理下游队列满的结果](#source-02) |
| 03 | [Logger 以整条记录入队，串行化多写入者](#source-03) |
| 04 | [TCP 部分发送保留未发送尾部](#source-04) |
| 05 | [整帧成功进入发送缓冲才推进订单序号](#source-05) |
| 06 | [TCP 区分 EOF，并继续处理背压留下的帧](#source-06) |
| 07 | [非阻塞连接检查真实完成结果](#source-07) |
| 08 | [内存池完整使用容量，并真正析构对象](#source-08) |
| 09 | [相同接收时间戳保持原到达顺序](#source-09) |
| 10 | [用完整方向和价格标识价位](#source-10) |
| 11 | [稀疏订单索引与修改前输入校验](#source-11) |
| 12 | [明确订单字节协议与会话身份](#source-12) |
| 13 | [撤单拒绝恢复订单状态，迟到确认不复活订单](#source-13) |
| 14 | [所有新单经过最终风控门，并预占待成交风险](#source-14) |
| 15 | [先去重回报，再更新持仓；不错误释放原订单风险](#source-15) |
| 16 | [快照需要全品种清空、同周期和状态哈希](#source-16) |
| 17 | [恢复批次整体发布，下游再核验簿](#source-17) |
| 18 | [线程持有任务和句柄，停止时真正 join](#source-18) |
| 19 | [信号处理只置标记，按依赖顺序排空](#source-19) |
| 20 | [特征按品种保存，先检查有效盘口](#source-20) |
| 21 | [关键交易审计独立于可丢诊断日志](#source-21) |
| 22 | [格式化时间使用调用内缓冲，移除共享成员写入](#source-22) |
| 23 | [客户端行情簿校验变更，CLEAR 同时释放并重置盘口](#source-23) |
| 24 | [UDP 保留未发送报文，并识别截断](#source-24) |
| 25 | [盘口汇总数量使用更宽类型](#source-25) |
| 26 | [进入撮合和发布回报前分别登记审计](#source-26) |

<a id="source-01"></a>

### 源码对照 01：队列满时拒绝写入，保留已有消息

旧接口总是给出可写指针，写完便回绕并增加元素数，没有容量检查。容量为 2 时第 3 条会覆盖旧消息。

**修改前**

[`common/lf_queue.h`，`c88c13b`，第 17–24 行](https://github.com/newbigdeng/TradeSystem/blob/c88c13b66e5c5d9f68ffb0848c27cbb527bb36b0/common/lf_queue.h#L17)

```cpp
    auto getNextToWriteTo() noexcept {
      return &store_[next_write_index_];
    }

    auto updateWriteIndex() noexcept {
      next_write_index_ = (next_write_index_ + 1) % store_.size();
      num_elements_++;
    }
```


**修改后**

[`common/lf_queue.h`，`5f70930`，第 16–24 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/common/lf_queue.h#L16)

```cpp
  bool try_push(const T &value) noexcept {
    const auto tail=tail_.load(std::memory_order_relaxed);
    const auto used=distance(tail,head_.load(std::memory_order_acquire));
    if(used==store_.size()) { full_count_.fetch_add(1,std::memory_order_relaxed); return false; }
    store_[tail%store_.size()]=value;
    tail_.store(next(tail),std::memory_order_release);
    if(used+1>high_watermark_.load(std::memory_order_relaxed)) high_watermark_.store(used+1,std::memory_order_relaxed);
    return true;
  }
```


**为什么这样改，以及如何验证：** 先用 acquire 读取消费者进度，满时返回 false；成功才复制并用 release 发布。索引按 2N 回绕，槽位按 N 映射。故障复现见 [baseline_results.json](baseline_results.json)，相同有效工作量的性能对照见 [perf_queue_before.json](perf_queue_before.json) / [perf_queue_after.json](perf_queue_after.json)。

<a id="source-02"></a>

### 源码对照 02：生产者处理下游队列满的结果

队列接口修复后，各链路也必须处理失败。FIFO 不能把未进入撮合队列的请求当作已交付；关键回报不能无声覆盖。

**修改前**

[`exchange/order_server/fifo_sequencer.h`，`c88c13b`，第 44–46 行](https://github.com/newbigdeng/TradeSystem/blob/c88c13b66e5c5d9f68ffb0848c27cbb527bb36b0/exchange/order_server/fifo_sequencer.h#L44)

```cpp
        auto next_write = incoming_requests_->getNextToWriteTo();
        *next_write = std::move(client_request.request_);
        incoming_requests_->updateWriteIndex();
```


**修改后**

[`exchange/order_server/fifo_sequencer.h`，`5f70930`，第 44–48 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/exchange/order_server/fifo_sequencer.h#L44)

```cpp
        if(!incoming_requests_->try_push(client_request.request_)) {
          std::move(pending_client_requests_.begin()+i,pending_client_requests_.begin()+pending_size_,pending_client_requests_.begin());
          pending_size_-=i;
          return;
        }
```

[`exchange/matcher/matching_engine.h`，`5f70930`，第 56–56 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/exchange/matcher/matching_engine.h#L56)

```cpp
      ASSERT(outgoing_ogw_responses_->try_push(std::move(*client_response)), "critical queue full; stop instead of overwriting");
```


**为什么这样改，以及如何验证：** FIFO 只移除成功发布的前缀，失败请求留待下轮；关键输出满则明确退出。后续 TCP 阶段再为接收帧补充可重试的背压。队列三轮百万序号检查和相关故障结果仍保留在本报告第四部分。

<a id="source-03"></a>

### 源码对照 03：Logger 以整条记录入队，串行化多写入者

旧 Logger 将文本拆成字符及数值逐个推入队列，多个线程可能交错，队列满也没有完整记录的失败结果。

**修改前**

[`common/logging.h`，`c88c13b`，第 115–118 行](https://github.com/newbigdeng/TradeSystem/blob/c88c13b66e5c5d9f68ffb0848c27cbb527bb36b0/common/logging.h#L115)

```cpp
    auto pushValue(const LogElement &log_element) noexcept {
      *(queue_.getNextToWriteTo()) = log_element;
      queue_.updateWriteIndex();
    }
```

[`common/logging.h`，`c88c13b`，第 156–161 行](https://github.com/newbigdeng/TradeSystem/blob/c88c13b66e5c5d9f68ffb0848c27cbb527bb36b0/common/logging.h#L156)

```cpp
    auto pushValue(const char *value) noexcept {
      while (*value) {
        pushValue(*value);
        ++value;
      }
    }
```


**修改后**

[`common/logging.h`，`5f70930`，第 32–37 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/common/logging.h#L32)

```cpp
  template<class... Args> void log(const char *format,const Args&... args) noexcept {
    std::ostringstream text; formatTo(text,format,args...);
    const auto record=text.str();
    std::lock_guard<std::mutex> lock(producers_);
    if(record.size()>65536 || !queue_.try_push(record)) dropped_.fetch_add(1,std::memory_order_relaxed);
  }
```


**为什么这样改，以及如何验证：** 先在调用内构造完整字符串，再用 producers_ 互斥锁串行化入队。过长或满队列时整条丢弃并计数；普通日志仍不是持久交易审计。相关队列与日志回归通过；后续关键审计使用独立组件，见对照 21。

<a id="source-04"></a>

### 源码对照 04：TCP 部分发送保留未发送尾部

非阻塞 send 可能只发送一部分或返回 EAGAIN。旧实现忽略实际发送字节数，随后无条件清空全部待发长度。

**修改前**

[`common/tcp_socket.cpp`，`5f70930`，第 46–51 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/common/tcp_socket.cpp#L46)

```cpp
    if (next_send_valid_index_ > 0) {
      // Non-blocking call to send data.
      const auto n = ::send(socket_fd_, outbound_data_.data(), next_send_valid_index_, MSG_DONTWAIT | MSG_NOSIGNAL);
      logger_.log("%:% %() % send socket:% len:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), socket_fd_, n);
    }
    next_send_valid_index_ = 0;
```


**修改后**

[`common/tcp_socket.cpp`，`33e804a`，第 20–29 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/tcp_socket.cpp#L20)

```cpp
  // Eight system calls per direction bound work and leave other connections time.
  for(int budget=0;budget<8 && next_send_valid_index_;++budget) {
    const auto n=::send(socket_fd_,outbound_data_.data()+send_begin_,next_send_valid_index_,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n>0) {send_begin_+=n;next_send_valid_index_-=n;sent_bytes_+=n;}
    else if(n<0 && errno==EINTR) continue;
    else if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {++send_would_block_;break;}
    else {last_error_=n==0?EPIPE:errno;state_=ConnectionState::Error;return false;}
  }
  if(!next_send_valid_index_) send_begin_=0;
  bool received=false;
```


**为什么这样改，以及如何验证：** 仅按 n>0 推进 send_begin_ 并减少 pending；EAGAIN 保留余量，EINTR 重试，每轮有调用预算。基准用相同完整 16 MiB 流；背压回归逐字节校验，见 [perf_tcp_before.json](perf_tcp_before.json) / [perf_tcp_after.json](perf_tcp_after.json) 及第四部分。

<a id="source-05"></a>

### 源码对照 05：整帧成功进入发送缓冲才推进订单序号

原来分两次追加序号和请求，追加没有失败返回；容量不足时无法保证完整帧。

**修改前**

[`trading/order_gw/order_gateway.cpp`，`5f70930`，第 25–31 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/trading/order_gw/order_gateway.cpp#L25)

```cpp
        tcp_socket_.send(&next_outgoing_seq_num_, sizeof(next_outgoing_seq_num_));
        tcp_socket_.send(client_request, sizeof(Exchange::MEClientRequest));
        END_MEASURE(Trading_TCPSocket_send, logger_);
        outgoing_requests_->pop();
        TTT_MEASURE(T12_OrderGateway_TCP_write, logger_);

        next_outgoing_seq_num_++;
```


**修改后**

[`trading/order_gw/order_gateway.cpp`，`33e804a`，第 29–37 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/trading/order_gw/order_gateway.cpp#L29)

```cpp
        const Exchange::OMClientRequest frame{next_outgoing_seq_num_,*client_request};
        const auto status=tcp_socket_.send(&frame,sizeof(frame));
        if(status==Common::SendResult::Full) break;
        ASSERT(status==Common::SendResult::Accepted,"order session unknown: cannot queue complete frame");
        END_MEASURE(Trading_TCPSocket_send, logger_);
        outgoing_requests_->pop();
        TTT_MEASURE(T12_OrderGateway_TCP_write, logger_);

        next_outgoing_seq_num_++;
```


**为什么这样改，以及如何验证：** 构造完整 OMClientRequest，Full 时保留原队列头，Accepted 后才 pop 和增加序号。协议改造阶段随后把这份结构改成明确字节协议，见对照 12。

<a id="source-06"></a>

### 源码对照 06：TCP 区分 EOF，并继续处理背压留下的帧

旧代码仅处理 recvmsg>0，没有可观察的 EOF/错误状态。下游满时保留的完整帧，也需要在没有新数据时继续尝试。

**修改前**

[`common/tcp_socket.cpp`，`5f70930`，第 26–28 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/common/tcp_socket.cpp#L26)

```cpp
    const auto read_size = recvmsg(socket_fd_, &msg, MSG_DONTWAIT);
    if (read_size > 0) {
      next_rcv_valid_index_ += read_size;
```


**修改后**

[`common/tcp_socket.cpp`，`33e804a`，第 30–33 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/tcp_socket.cpp#L30)

```cpp
  // Retry retained complete frames even without a fresh EPOLLIN edge.
  if(next_rcv_valid_index_ && recv_callback_) recv_callback_(this,last_receive_time_);
  for(int budget=0;budget<8 && healthy();++budget) {
    if(next_rcv_valid_index_==inbound_data_.size()) break; // downstream backpressure
```

[`common/tcp_socket.cpp`，`33e804a`，第 48–51 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/tcp_socket.cpp#L48)

```cpp
    } else if(n==0) {state_=ConnectionState::PeerClosed;break;}
    else if(errno==EINTR) continue;
    else if(errno==EAGAIN || errno==EWOULDBLOCK) break;
    else {last_error_=errno;state_=ConnectionState::Error;break;}
```


**为什么这样改，以及如何验证：** 旧片段仅展示接收分支入口，完整旧函数见版本链接。新实现把 EOF 标为 PeerClosed，把不可恢复错误标为 Error；接收回调会重试已有缓冲。端到端断线进入待对账状态，不自动重发经济订单。

<a id="source-07"></a>

### 源码对照 07：非阻塞连接检查真实完成结果

旧断言把 connect 返回值与 1 比较，connect 失败返回 -1 时反而通过；EINPROGRESS 也没有等待最终结果。

**修改前**

[`common/socket_utils.h`，`5f70930`，第 147–147 行](https://github.com/newbigdeng/TradeSystem/blob/5f70930ab547b2f9d1bebbd311fe09e938b82347/common/socket_utils.h#L147)

```cpp
        ASSERT(connect(socket_fd, rp->ai_addr, rp->ai_addrlen) != 1, "connect() failed. errno:" + std::string(strerror(errno)));
```


**修改后**

[`common/socket_utils.h`，`33e804a`，第 149–160 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/socket_utils.h#L149)

```cpp
        const auto connected=::connect(socket_fd,rp->ai_addr,rp->ai_addrlen);
        if(connected<0) {
          int error=errno;
          if(error==EINPROGRESS) {
            pollfd pending{socket_fd,POLLOUT,0}; int ready;
            do {ready=::poll(&pending,1,5000);} while(ready<0 && errno==EINTR);
            socklen_t size=sizeof(error);
            if(ready>0 && getsockopt(socket_fd,SOL_SOCKET,SO_ERROR,&error,&size)==0) {}
            else error=ready==0?ETIMEDOUT:errno;
          }
          if(error) {::close(socket_fd);socket_fd=-1;freeaddrinfo(result);errno=error;return -1;}
        }
```


**为什么这样改，以及如何验证：** EINPROGRESS 通过 poll 等待，再读 SO_ERROR；失败关闭 socket 并释放地址查询结果。成功退出路径也释放 getaddrinfo 结果。socket/epoll 的所有权与缓冲上限变更可从 [该次完整提交](https://github.com/newbigdeng/TradeSystem/commit/33e804a71260d187c52de39b9a08c915c7b92c07) 检查。

<a id="source-08"></a>

### 源码对照 08：内存池完整使用容量，并真正析构对象

旧 allocate 在占用当前槽后立即寻找下一个空槽；分配最后一个合法槽也会触发“无空间”。旧 deallocate 只标空，不析构对象。

**修改前**

[`common/mem_pool.h`，`33e804a`，第 20–32 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/mem_pool.h#L20)

```cpp
    T *allocate(Args... args) noexcept {
      auto obj_block = &(store_[next_free_index_]);
      ASSERT(obj_block->is_free_, "Expected free ObjectBlock at index:" + std::to_string(next_free_index_));
      T *ret = &(obj_block->object_);
      ret = new(ret) T(args...); // placement new.
      obj_block->is_free_ = false;

      updateNextFreeIndex();

      return ret;
    }

    /// Return the object back to the pool by marking the block as free again.
```

[`common/mem_pool.h`，`33e804a`，第 55–65 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/mem_pool.h#L55)

```cpp
      const auto initial_free_index = next_free_index_;
      while (!store_[next_free_index_].is_free_) {
        ++next_free_index_;
        if (UNLIKELY(next_free_index_ == store_.size())) { // hardware branch predictor should almost always predict this to be false any ways.
          next_free_index_ = 0;
        }
        if (UNLIKELY(initial_free_index == next_free_index_)) {
          ASSERT(initial_free_index != next_free_index_, "Memory Pool out of space.");
        }
      }
    }
```

[`common/mem_pool.h`，`33e804a`，第 34–39 行](https://github.com/newbigdeng/TradeSystem/blob/33e804a71260d187c52de39b9a08c915c7b92c07/common/mem_pool.h#L34)

```cpp
    auto deallocate(const T *elem) noexcept {
      const auto elem_index = (reinterpret_cast<const ObjectBlock *>(elem) - &store_[0]);
      ASSERT(elem_index >= 0 && static_cast<size_t>(elem_index) < store_.size(), "Element being deallocated does not belong to this Memory pool.");
      ASSERT(!store_[elem_index].is_free_, "Expected in-use ObjectBlock at index:" + std::to_string(elem_index));
      store_[elem_index].is_free_ = true;
    }
```


**修改后**

[`common/mem_pool.h`，`f8b1471`，第 19–33 行](https://github.com/newbigdeng/TradeSystem/blob/f8b147145ed5f1078dfe196a3dd10711b43d29aa/common/mem_pool.h#L19)

```cpp
  template<class... Args> T *allocate(Args&&... args) {
    if(free_.empty()) return nullptr;
    const auto index=free_.back();
    auto *object=new(blocks_[index].storage) T(std::forward<Args>(args)...);
    free_.pop_back(); blocks_[index].used=true; return object;
  }
  void deallocate(const T *object) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(object), base=reinterpret_cast<uintptr_t>(blocks_.data());
    ASSERT(address>=base && address<base+blocks_.size()*sizeof(Block) && (address-base)%sizeof(Block)==0,"foreign pool pointer");
    const auto index=(address-base)/sizeof(Block);
    ASSERT(blocks_[index].used,"double pool release");
    std::destroy_at(const_cast<T*>(object)); blocks_[index].used=false; free_.push_back(index);
  }
  ~MemPool() {
    for(auto &b:blocks_) if(b.used) std::destroy_at(std::launder(reinterpret_cast<T*>(b.storage)));
```


**为什么这样改，以及如何验证：** 空闲槽表先判断是否为空；最后一槽成功，第 N+1 次返回 nullptr。归还时检查归属与重复释放，再 destroy_at。OptMemPool 改为同一实现别名，不将其当独立优化算法。见 [perf_pool_before.json](perf_pool_before.json) / [perf_pool_after.json](perf_pool_after.json) 与容量、析构计数回归。

<a id="source-09"></a>

### 源码对照 09：相同接收时间戳保持原到达顺序

原 std::sort 不保证相等元素的顺序，33 条相同时间戳请求复现了重排。

**修改前**

[`exchange/order_server/fifo_sequencer.h`，`f8b1471`，第 37–37 行](https://github.com/newbigdeng/TradeSystem/blob/f8b147145ed5f1078dfe196a3dd10711b43d29aa/exchange/order_server/fifo_sequencer.h#L37)

```cpp
      std::sort(pending_client_requests_.begin(), pending_client_requests_.begin() + pending_size_);
```


**修改后**

[`exchange/order_server/fifo_sequencer.h`，`cc98699`，第 37–37 行](https://github.com/newbigdeng/TradeSystem/blob/cc986999c93dd00cb9035ff6ddc29c11954b52e0/exchange/order_server/fifo_sequencer.h#L37)

```cpp
      std::stable_sort(pending_client_requests_.begin(), pending_client_requests_.begin() + pending_size_);
```


**为什么这样改，以及如何验证：** 继续按时间比较，相同时间保留收集顺序。见 [perf_fifo_before.json](perf_fifo_before.json) / [perf_fifo_after.json](perf_fifo_after.json)；本次幅度小，不认定显著提速。

<a id="source-10"></a>

### 源码对照 10：用完整方向和价格标识价位

旧主簿把 price%256 作为档位身份，100 与 356 落入同一槽。另一套实现也使用了缩减后的键，两个实现都需要修正。

**修改前**

[`exchange/matcher/me_order_book.h`，`cc98699`，第 78–85 行](https://github.com/newbigdeng/TradeSystem/blob/cc986999c93dd00cb9035ff6ddc29c11954b52e0/exchange/matcher/me_order_book.h#L78)

```cpp
    auto priceToIndex(Price price) const noexcept {
      return (price % ME_MAX_PRICE_LEVELS);
    }

    /// Fetch and return the MEOrdersAtPrice corresponding to the provided price.
    auto getOrdersAtPrice(Price price) const noexcept -> MEOrdersAtPrice * {
      return price_orders_at_price_.at(priceToIndex(price));
    }
```


**修改后**

[`exchange/matcher/me_order_book.h`，`cd36235`，第 93–96 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/exchange/matcher/me_order_book.h#L93)

```cpp
    auto getOrdersAtPrice(Price price,Side side) const noexcept -> MEOrdersAtPrice* {
      const auto found=price_orders_at_price_.find({side,price});
      return found==price_orders_at_price_.end()?nullptr:found->second;
    }
```

[`exchange/matcher/unordered_map_me_order_book.h`，`cd36235`，第 95–98 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/exchange/matcher/unordered_map_me_order_book.h#L95)

```cpp
    auto getOrdersAtPrice(Price price,Side side) const noexcept -> MEOrdersAtPrice* {
      const auto found=price_orders_at_price_.find({side,price});
      return found==price_orders_at_price_.end()?nullptr:found->second;
    }
```


**为什么这样改，以及如何验证：** 查找、增加、删除及优先级获取都使用完整 (side,price)。固定价格碰撞和参考模型比较见 [book_baseline_failures.json](book_baseline_failures.json)、[book_model_test.cpp](book_model_test.cpp)；性能基本持平，不能宣称撮合簿全面提速。

<a id="source-11"></a>

### 源码对照 11：稀疏订单索引与修改前输入校验

旧客户订单表为巨大固定指针数组；新增订单也没有先检查身份、字段、重复经济 ID 和剩余容量。

**修改前**

[`exchange/matcher/me_order.h`，`cc98699`，第 37–40 行](https://github.com/newbigdeng/TradeSystem/blob/cc986999c93dd00cb9035ff6ddc29c11954b52e0/exchange/matcher/me_order.h#L37)

```cpp
  typedef std::array<MEOrder *, ME_MAX_ORDER_IDS> OrderHashMap;

  /// Hash map from ClientId -> OrderId -> MEOrder.
  typedef std::array<OrderHashMap, ME_MAX_NUM_CLIENTS> ClientOrderHashMap;
```

[`exchange/matcher/me_order_book.cpp`，`cc98699`，第 94–96 行](https://github.com/newbigdeng/TradeSystem/blob/cc986999c93dd00cb9035ff6ddc29c11954b52e0/exchange/matcher/me_order_book.cpp#L94)

```cpp
  auto MEOrderBook::add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty) noexcept -> void {
    const auto new_market_order_id = generateNewMarketOrderId();
    client_response_ = {ClientResponseType::ACCEPTED, client_id, ticker_id, client_order_id, new_market_order_id, side, price, 0, qty};
```


**修改后**

[`exchange/matcher/me_order.h`，`cd36235`，第 39–42 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/exchange/matcher/me_order.h#L39)

```cpp
  typedef std::unordered_map<OrderId,MEOrder*> OrderHashMap;

  /// Hash map from ClientId -> OrderId -> MEOrder.
  typedef std::array<OrderHashMap, ME_MAX_NUM_CLIENTS> ClientOrderHashMap;
```

[`exchange/matcher/me_order_book.cpp`，`cd36235`，第 95–105 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/exchange/matcher/me_order_book.cpp#L95)

```cpp
    RejectReason reason=RejectReason::NONE;
    if(client_id>=ME_MAX_NUM_CLIENTS || ticker_id!=ticker_id_ || !client_order_id || client_order_id>=ME_MAX_ORDER_IDS) reason=RejectReason::INVALID_ID;
    else if(side!=Side::BUY && side!=Side::SELL) reason=RejectReason::INVALID_SIDE;
    else if(price<=0 || price==Price_INVALID) reason=RejectReason::INVALID_PRICE;
    else if(!qty || qty==Qty_INVALID || qty>INT32_MAX) reason=RejectReason::INVALID_QTY;
    else if(used_client_order_ids_[client_id].contains(client_order_id)) reason=RejectReason::DUPLICATE_ID;
    else if(!order_pool_.available() || next_market_order_id_>=ME_MAX_ORDER_IDS || (!getOrdersAtPrice(price,side) && !orders_at_price_pool_.available())) reason=RejectReason::CAPACITY;
    if(reason!=RejectReason::NONE) {
      emitResponse({ClientResponseType::REJECTED,client_id,ticker_id,client_order_id,OrderId_INVALID,side,price,0,0,reason});return;
    }
    used_client_order_ids_[client_id].insert(client_order_id);
```


**为什么这样改，以及如何验证：** 客户表仅保存活动订单；所有输入先校验，拒绝后直接返回，防止数组越界或非法簿变更。历史 VmSize 下降不能解释为同等物理内存释放；RSS 仍基本不变，见原报告第四部分和 [book_probe.cpp](book_probe.cpp)。

<a id="source-12"></a>

### 源码对照 12：明确订单字节协议与会话身份

TCP 完整帧修复仍在传输 packed C++ 对象，缺少明确版本和字节序。新增协议不是把不存在的旧编码函数改名。

**修改前**

[`exchange/order_server/order_server.h`，`cd36235`，第 43–44 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/exchange/order_server/order_server.h#L43)

```cpp
          const OMClientResponse frame{next_outgoing_seq_num,*client_response};
          const auto status=cid_tcp_socket_[client_response->client_id_]->send(&frame,sizeof(frame));
```


**修改后**

[`common/order_protocol.h`，`e107084`，第 8–14 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/common/order_protocol.h#L8)

```cpp
constexpr uint8_t Version=1;
constexpr size_t RequestSize=50,ResponseSize=79;
template<size_t N> using Bytes=std::array<uint8_t,N>;
template<class T> void put(uint8_t*& output,T value) noexcept {
  using U=std::make_unsigned_t<T>;const U bits=std::bit_cast<U>(value);
  for(size_t i=sizeof(T);i>0;--i)*output++=static_cast<uint8_t>(bits>>((i-1)*8));
}
```

[`common/order_protocol.h`，`e107084`，第 23–26 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/common/order_protocol.h#L23)

```cpp
inline bool header(const uint8_t*& p,uint8_t kind,uint64_t& sequence,uint64_t& epoch) noexcept {
  if(get<uint8_t>(p)!='T' || get<uint8_t>(p)!='S' || get<uint8_t>(p)!=Version || get<uint8_t>(p)!=kind)return false;
  sequence=get<uint64_t>(p);epoch=get<uint64_t>(p);return true;
}
```

[`exchange/order_server/order_server.h`，`e107084`，第 46–47 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/exchange/order_server/order_server.h#L46)

```cpp
          const auto frame=Common::Wire::encode(OMClientResponse{next_outgoing_seq_num,*client_response,cid_session_epoch_[client_response->client_id_]});
          const auto status=cid_tcp_socket_[client_response->client_id_]->send(frame.data(),frame.size());
```


**为什么这样改，以及如何验证：** 新增固定宽度大端编码、魔数、版本、消息种类、会话代次和序号。请求 50 字节、回报 79 字节；接入端还检查客户与 socket 绑定、会话和序号。见 [protocol_baseline_failure.json](protocol_baseline_failure.json)、[protocol_test.cpp](protocol_test.cpp)；协议功能扩大带来额外成本，不与旧 memcpy 作等功能提速比较。

<a id="source-13"></a>

### 源码对照 13：撤单拒绝恢复订单状态，迟到确认不复活订单

旧 CANCEL_REJECTED 分支不改变状态，订单卡在 PENDING_CANCEL；ACCEPTED 无条件改为 LIVE，可能复活已经结束的订单。

**修改前**

[`trading/strategy/order_manager.h`，`cd36235`，第 32–35 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/order_manager.h#L32)

```cpp
        case Exchange::ClientResponseType::ACCEPTED: {
          order->order_state_ = OMOrderState::LIVE;
        }
          break;
```

[`trading/strategy/order_manager.h`，`cd36235`，第 47–50 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/order_manager.h#L47)

```cpp
        case Exchange::ClientResponseType::CANCEL_REJECTED:
        case Exchange::ClientResponseType::INVALID: {
        }
          break;
```


**修改后**

[`trading/strategy/order_manager.h`，`e107084`，第 33–35 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/strategy/order_manager.h#L33)

```cpp
        case Exchange::ClientResponseType::ACCEPTED:
          if(order->order_state_==OMOrderState::PENDING_NEW)order->order_state_=OMOrderState::LIVE;
          break;
```

[`trading/strategy/order_manager.h`，`e107084`，第 43–45 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/strategy/order_manager.h#L43)

```cpp
        case Exchange::ClientResponseType::CANCEL_REJECTED:
          if(order->order_state_==OMOrderState::PENDING_CANCEL)order->order_state_=client_response->leaves_qty_?OMOrderState::LIVE:OMOrderState::DEAD;
          break;
```

[`trading/strategy/order_manager.cpp`，`e107084`，第 9–12 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/strategy/order_manager.cpp#L9)

```cpp
    if(!trade_engine_->sendClientRequest(&new_request))return;

    *order = {ticker_id, next_order_id_, side, price, qty, OMOrderState::PENDING_NEW};
    ++next_order_id_;
```


**为什么这样改，以及如何验证：** 根据剩余量恢复 LIVE/DEAD；仅 PENDING_NEW 接受确认，发单失败不写入伪造的 PENDING_NEW 状态。故障输入与结果见 [control_baseline_failures.json](control_baseline_failures.json)、[control_probe.cpp](control_probe.cpp)。

<a id="source-14"></a>

### 源码对照 14：所有新单经过最终风控门，并预占待成交风险

旧风控检查主要位于 OrderManager，其他直接发单路径可以绕过；检查仅用已成交持仓，未加入尚未成交买单和卖单。

**修改前**

[`trading/strategy/trade_engine.cpp`，`cd36235`，第 64–68 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/trade_engine.cpp#L64)

```cpp
  auto TradeEngine::sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> void {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                client_request->toString().c_str());
    ASSERT(outgoing_ogw_requests_->try_push(std::move(*client_request)), "critical queue full; stop instead of overwriting");
    TTT_MEASURE(T10_TradeEngine_LFQueue_write, logger_);
```

[`trading/strategy/risk_manager.h`，`cd36235`，第 80–80 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/risk_manager.h#L80)

```cpp
      return ticker_risk_.at(ticker_id).checkPreTradeRisk(side, qty);
```

[`trading/strategy/risk_manager.h`，`cd36235`，第 52–52 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/risk_manager.h#L52)

```cpp
      if (UNLIKELY(std::abs(position_info_->position_ + sideToValue(side) * static_cast<int32_t>(qty)) > static_cast<int32_t>(risk_cfg_.max_position_)))
```


**修改后**

[`trading/strategy/trade_engine.cpp`，`e107084`，第 67–76 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/strategy/trade_engine.cpp#L67)

```cpp
    std::lock_guard<std::mutex> lock(state_mutex_);
    if(Wire::validate(*client_request)!=Exchange::RejectReason::NONE || client_request->client_id_!=client_id_ || !reconciled_ || (order_session_ && !order_session_->load(std::memory_order_acquire)) || (client_request->type_==Exchange::ClientRequestType::NEW && !market_trusted_.load(std::memory_order_acquire))) {
      ++rejected_requests_;logger_.log("ADMISSION REJECTED: invalid/stale/unreconciled/session-unknown\n");return false;
    }
    const auto risk=risk_manager_.reserve(*client_request);
    if(risk!=RiskCheckResult::ALLOWED) {++rejected_requests_;logger_.log("ADMISSION REJECTED: risk:%\n",riskCheckResultToString(risk));return false;}
    if(!outgoing_ogw_requests_->try_push(*client_request)) {risk_manager_.rollback(*client_request);++rejected_requests_;logger_.log("ADMISSION REJECTED: outgoing queue full\n");return false;}
    ++accepted_requests_;
    TTT_MEASURE(T10_TradeEngine_LFQueue_write, logger_);
    return true;
```

[`trading/strategy/risk_manager.h`，`e107084`，第 88–93 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/strategy/risk_manager.h#L88)

```cpp
      const auto position=int64_t(risk.position_info_->position_);
      const auto buys=pending_buy_[ticker_id]+(side==Side::BUY?qty:0);
      const auto sells=pending_sell_[ticker_id]+(side==Side::SELL?qty:0);
      if(position+int64_t(buys)>int64_t(risk.risk_cfg_.max_position_) || position-int64_t(sells)<-int64_t(risk.risk_cfg_.max_position_))return RiskCheckResult::POSITION_TOO_LARGE;
      if(!std::isfinite(risk.position_info_->total_pnl_))return RiskCheckResult::LOSS_TOO_LARGE;
      return RiskCheckResult::ALLOWED;
```


**为什么这样改，以及如何验证：** 最终发送口统一校验、检查行情可信/会话/对账状态、预占风险；入队失败回滚预占。分别计算全部买单或卖单成交的最坏持仓，不用两侧净额抵消风险。对应风控故障输入和性能循环见 [control_baseline_failures.json](control_baseline_failures.json)、[perf_admission_benchmark_before.json](perf_admission_benchmark_before.json) / [perf_admission_benchmark_after.json](perf_admission_benchmark_after.json)。

<a id="source-15"></a>

### 源码对照 15：先去重回报，再更新持仓；不错误释放原订单风险

旧成交回报直接进入 PositionKeeper，没有回报 ID 去重。初步风险预占实现又会在任何 REJECTED 时释放额度，需要区分重复经济 ID 拒绝。

**修改前**

[`trading/strategy/trade_engine.cpp`，`cd36235`，第 139–143 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/trade_engine.cpp#L139)

```cpp
    if (UNLIKELY(client_response->type_ == Exchange::ClientResponseType::FILLED)) {
      START_MEASURE(Trading_PositionKeeper_addFill);
      position_keeper_.addFill(client_response);
      END_MEASURE(Trading_PositionKeeper_addFill, logger_);
    }
```

[`trading/strategy/risk_manager.h`，`1c0af86`，第 120–122 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/strategy/risk_manager.h#L120)

```cpp
      } else if(r.type_==Exchange::ClientResponseType::CANCELED || r.type_==Exchange::ClientResponseType::REJECTED) {
        pending-=reservation.qty;reservations_.erase(found);
      }
```


**修改后**

[`trading/strategy/trade_engine.cpp`，`e107084`，第 150–159 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/strategy/trade_engine.cpp#L150)

```cpp
      if(client_response->response_id_) {
        if(client_response->response_id_<=last_response_id_) {++duplicate_responses_;return;}
        last_response_id_=client_response->response_id_;
      } else if(client_response->type_==Exchange::ClientResponseType::FILLED) {reconciled_=false;return;}
      if(!risk_manager_.onResponse(*client_response)) {reconciled_=false;logger_.log("RECONCILIATION REQUIRED: fill/reservation mismatch\n");return;}
      if(client_response->type_==Exchange::ClientResponseType::FILLED) position_keeper_.addFill(client_response);
      if(client_response->type_==Exchange::ClientResponseType::STATE && client_response->position_!=position_keeper_.getPositionInfo(client_response->ticker_id_)->position_) {
        reconciled_=false;logger_.log("RECONCILIATION REQUIRED: exchange/local position mismatch\n");return;
      }
    }
```

[`trading/strategy/risk_manager.h`，`701007c`，第 120–126 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/trading/strategy/risk_manager.h#L120)

```cpp
      } else if(r.type_==Exchange::ClientResponseType::CANCEL_REJECTED) {
        if(r.leaves_qty_!=reservation.qty)return false;
      } else if(r.type_==Exchange::ClientResponseType::REJECTED && r.reject_reason_==Exchange::RejectReason::DUPLICATE_ID) {
        return false; // The original economic order may still be live.
      } else if(r.type_==Exchange::ClientResponseType::CANCELED || r.type_==Exchange::ClientResponseType::REJECTED) {
        pending-=reservation.qty;reservations_.erase(found);
      }
```


**为什么这样改，以及如何验证：** 回报 ID 已见则提前返回，成交数量与剩余数量先对账；重复经济 ID 的拒绝不能证明原订单已结束，因而进入待对账并保留风险。撤单拒绝的剩余量不一致也拒绝继续交易。两块修改属于不同提交，版本链接分别标出；相关控制、协议与集成回归见原报告第四部分。

<a id="source-16"></a>

### 源码对照 16：快照需要全品种清空、同周期和状态哈希

旧恢复依赖 START、END 与局部连续序号，缺少每品种 CLEAR、周期、水位及状态一致性证明。

**修改前**

[`trading/market_data/market_data_consumer.cpp`，`e107084`，第 88–93 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/market_data/market_data_consumer.cpp#L88)

```cpp
    const auto &last_snapshot_msg = snapshot_queued_msgs_.rbegin()->second;
    if (last_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_END) {
      logger_.log("%:% %() % Returning because have not seen a SNAPSHOT_END yet.\n",
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      return;
    }
```

[`trading/market_data/market_data_consumer.cpp`，`e107084`，第 139–139 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/market_data/market_data_consumer.cpp#L139)

```cpp
    in_recovery_ = false;
```


**修改后**

[`trading/market_data/market_recovery.h`，`1c0af86`，第 51–61 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/market_data/market_recovery.h#L51)

```cpp
  std::optional<Batch> build() {
    if(snapshot_.empty() || invalid_cycle_)return std::nullopt;
    const auto& first=snapshot_.begin()->second;const auto& last=snapshot_.rbegin()->second;
    if(first.seq_num_!=0 || first.me_market_update_.type_!=Type::SNAPSHOT_START || last.me_market_update_.type_!=Type::SNAPSHOT_END)return std::nullopt;
    const auto watermark=first.watermark_;
    if(last.watermark_!=watermark || first.me_market_update_.order_id_!=watermark || last.me_market_update_.order_id_!=watermark || first.state_hash_!=last.state_hash_) {invalid_cycle_=true;++invalid_;return std::nullopt;}
    std::array<bool,Common::ME_MAX_TICKERS> cleared{};
    State state;Batch batch{{},watermark,0};uint64_t sequence=0;
    for(const auto& [id,wrapped]:snapshot_) {
      if(id!=sequence++ || wrapped.watermark_!=watermark || wrapped.snapshot_cycle_!=cycle_ || wrapped.state_hash_!=first.state_hash_)return std::nullopt;
      const auto& event=wrapped.me_market_update_;
```

[`trading/market_data/market_recovery.h`，`1c0af86`，第 72–74 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/market_data/market_recovery.h#L72)

```cpp
    if(!std::all_of(cleared.begin(),cleared.end(),[](bool value){return value;}))return std::nullopt;
    if(Common::bookHash(flatten(state))!=first.state_hash_) {++hash_failures_;invalid_cycle_=true;return std::nullopt;}
    uint64_t next=watermark+1;
```


**为什么这样改，以及如何验证：** 新 MarketRecovery 是提取后的独立状态机；缺少任何品种 CLEAR、周期/水位不一致、缺口或哈希错误都不 build 成功。旧实现不是把相同校验函数改名。见 [recovery_baseline_failure.json](recovery_baseline_failure.json)、[recovery_test.cpp](recovery_test.cpp)；组件耗时不是从真实网络缺包到恢复交易的总时间。

<a id="source-17"></a>

### 源码对照 17：恢复批次整体发布，下游再核验簿

旧代码逐条推恢复事件，队列中途满会留下半批。新实现先完成内部重建，再一次发布全部事件及提交标记。

**修改前**

[`trading/market_data/market_data_consumer.cpp`，`e107084`，第 130–132 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/trading/market_data/market_data_consumer.cpp#L130)

```cpp
    for (const auto &itr: final_events) {
      ASSERT(incoming_md_updates_->try_push(itr), "critical queue full; stop instead of overwriting");
    }
```


**修改后**

[`common/lf_queue.h`，`1c0af86`，第 30–39 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/common/lf_queue.h#L30)

```cpp
  bool try_push_batch(const std::vector<T>& values) noexcept {
    const auto tail=tail_.load(std::memory_order_relaxed);
    const auto used=distance(tail,head_.load(std::memory_order_acquire));
    if(values.size()>store_.size()-used) {full_count_.fetch_add(1,std::memory_order_relaxed);return false;}
    auto write=tail;
    for(const auto& value:values) {store_[write%store_.size()]=value;write=next(write);}
    tail_.store(write,std::memory_order_release);
    if(used+values.size()>high_watermark_.load(std::memory_order_relaxed))high_watermark_.store(used+values.size(),std::memory_order_relaxed);
    return true;
  }
```

[`trading/strategy/trade_engine.cpp`，`1c0af86`，第 98–106 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/strategy/trade_engine.cpp#L98)

```cpp
        if(market_update->type_==Exchange::MarketUpdateType::RECOVERY_COMMIT) {
          std::vector<Exchange::MEMarketUpdate> orders;
          for(const auto* book:ticker_order_book_) {const auto live=book->liveOrders();orders.insert(orders.end(),live.begin(),live.end());}
          const auto hash=Common::bookHash(orders);
          const bool verified=hash==market_update->state_hash_ && market_update->priority_==market_generation_.load(std::memory_order_acquire);
          market_trusted_.store(verified,std::memory_order_release);
          logger_.log("RECOVERY VERIFIED:% watermark:% hash:% expected:% generation:%\n",verified,market_update->order_id_,hash,market_update->state_hash_,market_update->priority_);
          if(!verified)market_generation_.fetch_add(1,std::memory_order_acq_rel);
          incoming_md_updates_->pop();continue;
```


**为什么这样改，以及如何验证：** 批次容量先检查，写完才 release 发布 tail；TradeEngine 按同一水位与恢复代次计算本地簿哈希，通过后才设为可信。批次不足重试，不发布半批。原报告的恢复与集成验收保留其原有范围。

<a id="source-18"></a>

### 源码对照 18：线程持有任务和句柄，停止时真正 join

旧创建函数引用捕获调用内对象并固定等一秒；部分模块只改运行标记、在析构时等待一秒，不能证明线程已停止。

**修改前**

[`common/thread_utils.h`，`1c0af86`，第 25–33 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/common/thread_utils.h#L25)

```cpp
    auto t = new std::thread([&]() {
      if (core_id >= 0 && !setThreadCore(core_id)) {
        std::cerr << "Failed to set core affinity for " << name << " " << pthread_self() << " to " << core_id << std::endl;
        exit(EXIT_FAILURE);
      }
      std::cerr << "Set core affinity for " << name << " " << pthread_self() << " to " << core_id << std::endl;

      std::forward<T>(func)((std::forward<A>(args))...);
    });
```

[`common/thread_utils.h`，`1c0af86`，第 36–36 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/common/thread_utils.h#L36)

```cpp
    std::this_thread::sleep_for(1s);
```

[`exchange/order_server/order_server.cpp`，`1c0af86`，第 30–32 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/exchange/order_server/order_server.cpp#L30)

```cpp
  auto OrderServer::stop() -> void {
    run_ = false;
  }
```


**修改后**

[`common/thread_utils.h`，`701007c`，第 28–33 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/common/thread_utils.h#L28)

```cpp
    auto task=std::bind(std::forward<T>(func),std::forward<A>(args)...);
    auto t=new std::thread([core_id,name,task=std::move(task)]() mutable {
      if(core_id>=0 && !setThreadCore(core_id)) FATAL("cannot pin thread: "+name);
      task();
    });
    return t;
```

[`exchange/order_server/order_server.cpp`，`701007c`，第 28–31 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/order_server/order_server.cpp#L28)

```cpp
  auto OrderServer::stop() -> void {
    accepting_.store(false);stop_deadline_=Common::getMonotonicNanos()+5*NANOS_TO_SECS;run_=false;
    if(worker_ && worker_->joinable())worker_->join();
  }
```


**为什么这样改，以及如何验证：** 任务及名字按值保存在工作线程，去掉固定等待；对象持有 worker_，停止设置原子标记后 join。生命周期测量见 [perf_lifecycle_before.json](perf_lifecycle_before.json) / [perf_lifecycle_after.json](perf_lifecycle_after.json)；99.95% 主要是去掉一秒，不代表订单处理缩短一秒。

<a id="source-19"></a>

### 源码对照 19：信号处理只置标记，按依赖顺序排空

旧信号处理函数睡眠并直接析构模块，停止顺序不能保证请求、撮合、行情与回报已经交付。

**修改前**

[`exchange/exchange_main.cpp`，`1c0af86`，第 14–29 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/exchange/exchange_main.cpp#L14)

```cpp
void signal_handler(int) {
  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);

  delete logger;
  logger = nullptr;
  delete matching_engine;
  matching_engine = nullptr;
  delete market_data_publisher;
  market_data_publisher = nullptr;
  delete order_server;
  order_server = nullptr;

  std::this_thread::sleep_for(10s);

  exit(EXIT_SUCCESS);
```


**修改后**

[`exchange/exchange_main.cpp`，`701007c`，第 6–8 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/exchange_main.cpp#L6)

```cpp
namespace {volatile std::sig_atomic_t stopping=0;void signalHandler(int){stopping=1;}}
int main() {
  std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);
```

[`exchange/exchange_main.cpp`，`701007c`，第 19–19 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/exchange_main.cpp#L19)

```cpp
  server->quiesce();engine->stop();publisher->stop();server->stop();
```

[`exchange/order_server/order_server.h`，`701007c`，第 38–40 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/order_server/order_server.h#L38)

```cpp
      while(run_ || outgoing_responses_->peek() || tcp_server_.pendingBytes()) {
        if(!run_ && Common::getMonotonicNanos()>stop_deadline_.load())FATAL("shutdown has unacknowledged exchange output; reconciliation required");
        if(accepting_.load(std::memory_order_acquire))tcp_server_.poll();
```


**为什么这样改，以及如何验证：** 主线程先停止接单并排空 FIFO，再停止撮合、行情发布，最后排空回报。输出超过单调时钟期限仍未排空则明确失败。SIGTERM 退出从 -15 到 0 的历史结果见 [perf_e2e_before.json](perf_e2e_before.json) / [perf_e2e_after.json](perf_e2e_after.json)，未将排空成功解释为任意崩溃自动恢复。

<a id="source-20"></a>

### 源码对照 20：特征按品种保存，先检查有效盘口

旧 FeatureEngine 用两个标量保存所有品种特征，第二品种覆盖第一品种；算术在整数乘加后才转浮点，盘口缺失时还可能保留旧值。

**修改前**

[`trading/strategy/feature_engine.h`，`1c0af86`，第 22–22 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/strategy/feature_engine.h#L22)

```cpp
        mkt_price_ = (bbo->bid_price_ * bbo->ask_qty_ + bbo->ask_price_ * bbo->bid_qty_) / static_cast<double>(bbo->bid_qty_ + bbo->ask_qty_);
```

[`trading/strategy/feature_engine.h`，`1c0af86`，第 66–66 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/strategy/feature_engine.h#L66)

```cpp
    double mkt_price_ = Feature_INVALID, agg_trade_qty_ratio_ = Feature_INVALID;
```


**修改后**

[`trading/strategy/feature_engine.h`，`701007c`，第 10–18 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/trading/strategy/feature_engine.h#L10)

```cpp
  void onOrderBookUpdate(Common::TickerId ticker,Common::Price price,Common::Side side,MarketOrderBook* book) noexcept {
    if(ticker>=Common::ME_MAX_TICKERS)return;
    const auto* bbo=book->getBBO();prices_[ticker]=Feature_INVALID;
    if(valid(*bbo)) {
      const double denominator=double(bbo->bid_qty_)+double(bbo->ask_qty_);
      prices_[ticker]=(double(bbo->bid_price_)*double(bbo->ask_qty_)+double(bbo->ask_price_)*double(bbo->bid_qty_))/denominator;
      if(!std::isfinite(prices_[ticker]))prices_[ticker]=Feature_INVALID;
    } else ratios_[ticker]=Feature_INVALID;
    logger_->log("FEATURE ticker:% price:% side:% fair:% ratio:%\n",ticker,price,int(side),prices_[ticker],ratios_[ticker]);
```

[`trading/strategy/feature_engine.h`，`701007c`，第 33–35 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/trading/strategy/feature_engine.h#L33)

```cpp
  static bool valid(const BBO& bbo) noexcept {
    return bbo.bid_price_>0 && bbo.ask_price_>0 && bbo.bid_price_!=Common::Price_INVALID && bbo.ask_price_!=Common::Price_INVALID && bbo.bid_qty_>0 && bbo.ask_qty_>0 && bbo.bid_qty_!=BookQty_INVALID && bbo.ask_qty_!=BookQty_INVALID;
  }
```

[`trading/strategy/feature_engine.h`，`701007c`，第 37–37 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/trading/strategy/feature_engine.h#L37)

```cpp
  std::array<double,Common::ME_MAX_TICKERS> prices_,ratios_;
```


**为什么这样改，以及如何验证：** 每品种独立数组；运算前显式转 double，盘口无效或结果非有限值时设 NaN；交易策略也检查 isfinite。BookQty 与持仓宽度、零深度保护及单调截止时间见 [该次提交](https://github.com/newbigdeng/TradeSystem/commit/701007c7a618eef93bf9ca1a5db0b231041c36ea)。逐品种故障与性能结果见 [control_baseline_failures.json](control_baseline_failures.json)、[perf_feature_benchmark_before.json](perf_feature_benchmark_before.json) / [perf_feature_benchmark_after.json](perf_feature_benchmark_after.json)。

<a id="source-21"></a>

### 源码对照 21：关键交易审计独立于可丢诊断日志

原来只有普通 Logger，没有关键交易事件的同步持久化契约，也没有防止覆盖旧审计文件的重启门。此处属于新增可靠性功能。

**修改前**

[`exchange/exchange_main.cpp`，`1c0af86`，第 33–33 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/exchange/exchange_main.cpp#L33)

```cpp
  logger = new Common::Logger("exchange_main.log");
```


**修改后**

[`common/critical_journal.h`，`701007c`，第 15–24 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/common/critical_journal.h#L15)

```cpp
  explicit CriticalJournal(const std::string& path) {
    fd_=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    ASSERT(fd_>=0,"existing/unavailable order journal: reconcile or explicitly archive the previous simulation before restarting: "+path);
    append("TS_AUDIT_V1");
    const auto slash=path.find_last_of('/');const auto directory=slash==std::string::npos?std::string("."):path.substr(0,slash);
    const int parent=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ASSERT(parent>=0,"cannot open journal parent directory");
    ASSERT(::fsync(parent)==0,"cannot persist journal directory entry");::close(parent);
  }
  ~CriticalJournal(){if(fd_>=0)::close(fd_);}
```

[`common/critical_journal.h`，`701007c`，第 25–33 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/common/critical_journal.h#L25)

```cpp
  void append(const std::string& text) {
    std::lock_guard<std::mutex> lock(writer_);const auto record=text+'\n';size_t done=0;
    while(done<record.size()) {
      const auto n=::write(fd_,record.data()+done,record.size()-done);
      if(n>0)done+=n;else if(n<0&&errno==EINTR)continue;else FATAL("critical journal write failed");
    }
    int rc;do {rc=::fsync(fd_);}while(rc<0&&errno==EINTR);
    ASSERT(rc==0,"critical journal fsync failed");++records_;
  }
```

[`exchange/order_server/order_server.h`，`701007c`，第 124–125 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/order_server/order_server.h#L124)

```cpp
          if(audit_)audit_->request("RECEIVED",r);
          ASSERT(fifo_sequencer_.addClientRequest(rx_time,r),"FIFO capacity changed within single owner");
```


**为什么这样改，以及如何验证：** O_EXCL 拒绝覆盖已有审计；write 处理短写和 EINTR，每条之后 fsync，创建时同步父目录。正常请求还记录 APPLY 与 RESPONSE，只读 reconcile 校验守恒；APPLY 不是事务提交、也不自动重放。见 [journal_test.cpp](journal_test.cpp)、[reconcile.py](reconcile.py) 和端到端成本对照。

<a id="source-22"></a>

### 源码对照 22：格式化时间使用调用内缓冲，移除共享成员写入

ctime 使用共享内部缓冲；业务入口在 Logger 加锁之前还会写入同一 time_str_，仅锁 Logger 不能消除这两处竞争。

**修改前**

[`common/time_utils.h`，`701007c`，第 30–39 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/common/time_utils.h#L30)

```cpp
  inline auto& getCurrentTimeStr(std::string* time_str) {
    const auto clock = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(clock);

    char nanos_str[24];
    sprintf(nanos_str, "%.8s.%09ld", ctime(&time) + 11, std::chrono::duration_cast<std::chrono::nanoseconds>(clock.time_since_epoch()).count() % NANOS_TO_SECS);
    time_str->assign(nanos_str);

    return *time_str;
  }
```

[`trading/strategy/trade_engine.cpp`，`701007c`，第 61–63 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/trading/strategy/trade_engine.cpp#L61)

```cpp
  auto TradeEngine::sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> bool {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                client_request->toString().c_str());
```


**修改后**

[`common/time_utils.h`，`9572484`，第 30–40 行](https://github.com/newbigdeng/TradeSystem/blob/957248424dfc34a50d689c36672cd00907d61ff0/common/time_utils.h#L30)

```cpp
  inline std::string getCurrentTimeStr() {
    const auto clock = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(clock);

    char calendar[26],nanos_str[24];
    ctime_r(&time,calendar);
    snprintf(nanos_str,sizeof(nanos_str),"%.8s.%09lld",calendar+11,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock.time_since_epoch()).count()%NANOS_TO_SECS));
    return nanos_str;
  }
  // Compatibility for single-owner examples and preserved measurement probes.
```

[`trading/strategy/trade_engine.cpp`，`9572484`，第 61–63 行](https://github.com/newbigdeng/TradeSystem/blob/957248424dfc34a50d689c36672cd00907d61ff0/trading/strategy/trade_engine.cpp#L61)

```cpp
  auto TradeEngine::sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> bool {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                client_request->toString().c_str());
```


**为什么这样改，以及如何验证：** ctime_r 写调用内 calendar，返回独立字符串，业务调用不再传共享成员指针。兼容重载仍供单所有者示例使用。见 [concurrent_baseline_failure.json](concurrent_baseline_failure.json)、[perf_clock_before.json](perf_clock_before.json) / [perf_clock_after.json](perf_clock_after.json) 及独立 TSan 回归结果。

<a id="source-23"></a>

### 源码对照 23：客户端行情簿校验变更，CLEAR 同时释放并重置盘口

旧客户端簿在检查字段前就访问订单指针；CLEAR 虽清理订单，但档位索引和 BBO 更新没有完整契约。

**修改前**

[`trading/strategy/market_order_book.cpp`，`cc98699`，第 20–22 行](https://github.com/newbigdeng/TradeSystem/blob/cc986999c93dd00cb9035ff6ddc29c11954b52e0/trading/strategy/market_order_book.cpp#L20)

```cpp
  auto MarketOrderBook::onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> void {
    const auto bid_updated = (bids_by_price_ && market_update->side_ == Side::BUY && market_update->price_ >= bids_by_price_->price_);
    const auto ask_updated = (asks_by_price_ && market_update->side_ == Side::SELL && market_update->price_ <= asks_by_price_->price_);
```

[`trading/strategy/market_order_book.cpp`，`cc98699`，第 79–79 行](https://github.com/newbigdeng/TradeSystem/blob/cc986999c93dd00cb9035ff6ddc29c11954b52e0/trading/strategy/market_order_book.cpp#L79)

```cpp
    updateBBO(bid_updated, ask_updated);
```


**修改后**

[`trading/strategy/market_order_book.cpp`，`cd36235`，第 20–29 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/market_order_book.cpp#L20)

```cpp
  auto MarketOrderBook::onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> bool {
    if(market_update->ticker_id_!=ticker_id_) return false;
    const auto type=market_update->type_;
    if(type==Exchange::MarketUpdateType::ADD || type==Exchange::MarketUpdateType::MODIFY || type==Exchange::MarketUpdateType::CANCEL) {
      if(!market_update->order_id_ || market_update->order_id_>=ME_MAX_ORDER_IDS) return false;
      const auto *existing=oid_to_order_[market_update->order_id_];
      if(type==Exchange::MarketUpdateType::ADD) {
        if(existing || !order_pool_.available() || (market_update->side_!=Side::BUY && market_update->side_!=Side::SELL) || market_update->price_<=0 || market_update->price_==Price_INVALID || !market_update->qty_ || market_update->qty_==Qty_INVALID || (!getOrdersAtPrice(market_update->price_,market_update->side_) && !orders_at_price_pool_.available())) return false;
      } else if(!existing || existing->side_!=market_update->side_ || existing->price_!=market_update->price_ || (type==Exchange::MarketUpdateType::MODIFY && (!market_update->qty_ || market_update->qty_==Qty_INVALID))) return false;
    }
```

[`trading/strategy/market_order_book.cpp`，`cd36235`，第 64–66 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/market_order_book.cpp#L64)

```cpp
        for(const auto &[key,level]:price_orders_at_price_) { (void)key;orders_at_price_pool_.deallocate(level); }
        price_orders_at_price_.clear();
        bids_by_price_ = asks_by_price_ = nullptr;
```

[`trading/strategy/market_order_book.cpp`，`cd36235`，第 76–76 行](https://github.com/newbigdeng/TradeSystem/blob/cd36235dee36ec5704dcc5a8bfc324383b8471c3/trading/strategy/market_order_book.cpp#L76)

```cpp
    updateBBO(true, true);
```


**为什么这样改，以及如何验证：** ADD 校验唯一订单、字段与容量，MODIFY/CANCEL 核对已有订单方向及价格；CLEAR 清理完整价位映射并重置两侧。返回 false 后，后续 TradeEngine 可信度链路会触发重新恢复。对应客户端簿及恢复回归仍见原报告第四部分。

<a id="source-24"></a>

### 源码对照 24：UDP 保留未发送报文，并识别截断

旧组播 socket 在一次 send 后无条件清空长度，接收用 recv 也不能明确标识被截断的数据报。这与 TCP 流的部分写入语义不同。

**修改前**

[`common/mcast_socket.cpp`，`e107084`，第 36–41 行](https://github.com/newbigdeng/TradeSystem/blob/e10708417f917fcae3a6594ca6a70b6e8c02f41d/common/mcast_socket.cpp#L36)

```cpp
    if (next_send_valid_index_ > 0) {
      ssize_t n = ::send(socket_fd_, outbound_data_.data(), next_send_valid_index_, MSG_DONTWAIT | MSG_NOSIGNAL);

      logger_.log("%:% %() % send socket:% len:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), socket_fd_, n);
    }
    next_send_valid_index_ = 0;
```


**修改后**

[`common/mcast_socket.cpp`，`1c0af86`，第 11–16 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/common/mcast_socket.cpp#L11)

```cpp
  if(next_send_valid_index_) {
    const auto n=::send(socket_fd_,outbound_data_.data(),next_send_valid_index_,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==static_cast<ssize_t>(next_send_valid_index_)) {sent_bytes_+=n;next_send_valid_index_=0;}
    else if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))++send_retries_;
    else FATAL("multicast send failure: "+std::string(strerror(errno)));
  }
```

[`common/mcast_socket.cpp`，`1c0af86`，第 17–24 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/common/mcast_socket.cpp#L17)

```cpp
  iovec iov{inbound_data_.data(),inbound_data_.size()};msghdr message{};message.msg_iov=&iov;message.msg_iovlen=1;
  const auto n=recvmsg(socket_fd_,&message,MSG_DONTWAIT|MSG_TRUNC);
  if(n>0) {
    receive_fault_=(message.msg_flags&MSG_TRUNC) || size_t(n)>inbound_data_.size();
    next_rcv_valid_index_=receive_fault_?0:size_t(n);
    if(receive_fault_)++truncated_datagrams_;else received_bytes_+=n;
    if(recv_callback_)recv_callback_(this);
    return true;
```


**为什么这样改，以及如何验证：** UDP 必须整报文接受才清空；EAGAIN/EINTR 保留报文，其他异常明确退出。recvmsg 配合 MSG_TRUNC 标识截断，行情接收器把该状态转为不可信并恢复；未把它称为网络一定不丢包。完整发送边界与接收恢复源码见该次提交。

<a id="source-25"></a>

### 源码对照 25：盘口汇总数量使用更宽类型

订单单笔数量与价位汇总数量原来共用 Qty；多个大数量订单相加，汇总值需要独立的更宽范围和无效值。

**修改前**

[`trading/strategy/market_order.h`，`1c0af86`，第 70–72 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/trading/strategy/market_order.h#L70)

```cpp
  struct BBO {
    Price bid_price_ = Price_INVALID, ask_price_ = Price_INVALID;
    Qty bid_qty_ = Qty_INVALID, ask_qty_ = Qty_INVALID;
```


**修改后**

[`trading/strategy/market_order.h`，`701007c`，第 70–74 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/trading/strategy/market_order.h#L70)

```cpp
  using BookQty=uint64_t;
  constexpr BookQty BookQty_INVALID=UINT64_MAX;
  struct BBO {
    Price bid_price_ = Price_INVALID, ask_price_ = Price_INVALID;
    BookQty bid_qty_ = BookQty_INVALID, ask_qty_ = BookQty_INVALID;
```


**为什么这样改，以及如何验证：** BookQty 为 uint64_t，BBO 无效值也改成 BookQty_INVALID；与特征运算前转 double、风险运算前转 int64_t 配套。数值与零深度检查在可靠性回归中验收；这属于数值边界修复，没有单独宣称吞吐收益。

<a id="source-26"></a>

### 源码对照 26：进入撮合和发布回报前分别登记审计

新增审计必须接入关键链路，不能只创建一个文件。下列两处分别展示原来没有审计调用与补充后的调用位置。

**修改前**

[`exchange/matcher/matching_engine.h`，`1c0af86`，第 29–30 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/exchange/matcher/matching_engine.h#L29)

```cpp
    auto processClientRequest(const MEClientRequest *client_request) noexcept {
      const auto invalid=Common::Wire::validate(*client_request);
```

[`exchange/matcher/matching_engine.h`，`1c0af86`，第 70–70 行](https://github.com/newbigdeng/TradeSystem/blob/1c0af8688bd2a4a2d72541a1ac05763780b93521/exchange/matcher/matching_engine.h#L70)

```cpp
      ASSERT(outgoing_ogw_responses_->try_push(response), "response queue full: matching fail-closed; reconciliation required");
```


**修改后**

[`exchange/matcher/matching_engine.h`，`701007c`，第 30–31 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/matcher/matching_engine.h#L30)

```cpp
    auto processClientRequest(const MEClientRequest *client_request) noexcept {
      if(audit_)audit_->request("APPLY",*client_request);
```

[`exchange/matcher/matching_engine.h`，`701007c`，第 72–73 行](https://github.com/newbigdeng/TradeSystem/blob/701007c7a618eef93bf9ca1a5db0b231041c36ea/exchange/matcher/matching_engine.h#L72)

```cpp
      if(audit_)audit_->response(response);
      ASSERT(outgoing_ogw_responses_->try_push(response), "response queue full: matching fail-closed; reconciliation required");
```


**为什么这样改，以及如何验证：** 处理入口记录 APPLY，回报入队前记录 RESPONSE，接入点的 RECEIVED 见对照 21。三个阶段可由请求身份和回报身份关联，对账工具只检查记录和数量关系，不把 APPLY 当完整事务提交。历史 fsync 成本与停机结果继续保留。

### 完整提交与配套测试

正文聚焦产生问题和改变行为的关键源码；同次提交的调用方迁移、类型定义、构建配置及新增回归测试在以下完整提交中保留。新增测试或工具没有旧实现，不为它们虚构“修改前代码”。

- [`5f70930`：Bound SPSC queues and diagnostic log records; retain critical overflow](https://github.com/newbigdeng/TradeSystem/commit/5f70930ab547b2f9d1bebbd311fe09e938b82347)
- [`33e804a`：Retain partial TCP sends and apply frame-level backpressure](https://github.com/newbigdeng/TradeSystem/commit/33e804a71260d187c52de39b9a08c915c7b92c07)
- [`f8b1471`：Make all pool slots usable and destroy released objects](https://github.com/newbigdeng/TradeSystem/commit/f8b147145ed5f1078dfe196a3dd10711b43d29aa)
- [`cc98699`：Preserve FIFO arrival order when kernel timestamps tie](https://github.com/newbigdeng/TradeSystem/commit/cc986999c93dd00cb9035ff6ddc29c11954b52e0)
- [`cd36235`：Use complete price keys and sparse client order tables; validate book mutations](https://github.com/newbigdeng/TradeSystem/commit/cd36235dee36ec5704dcc5a8bfc324383b8471c3)
- [`e107084`：Validate versioned sessions, restore order state and reserve pending risk](https://github.com/newbigdeng/TradeSystem/commit/e10708417f917fcae3a6594ca6a70b6e8c02f41d)
- [`1c0af86`：Fence snapshot recovery with cycle IDs, state hashes and atomic batch publication](https://github.com/newbigdeng/TradeSystem/commit/1c0af8688bd2a4a2d72541a1ac05763780b93521)
- [`701007c`：Drain owned workers, isolate features, persist critical order audit and fence unsafe restarts](https://github.com/newbigdeng/TradeSystem/commit/701007c7a618eef93bf9ca1a5db0b231041c36ea)
- [`9572484`：Remove shared timestamp buffers and verify concurrency with separate sanitizer builds](https://github.com/newbigdeng/TradeSystem/commit/957248424dfc34a50d689c36672cd00907d61ff0)
