# 06 源码对照

下面只列关键修改，完整实现见各文件。网络和协议的处理顺序未调整；注释部分另外做了代码标记比对，避免误删语句。

## 1. 定序器：第一条就遇到满队列时不搬移

文件：[fifo_sequencer.h](../../exchange/order_server/fifo_sequencer.h)

原来：

```cpp
std::move(pending_client_requests_.begin() + i,
          pending_client_requests_.begin() + pending_size_,
          pending_client_requests_.begin());
pending_size_ -= i;
```

现在：

```cpp
if (i > 0) {
    std::move(pending_client_requests_.begin() + i,
              pending_client_requests_.begin() + pending_size_,
              pending_client_requests_.begin());
}
pending_size_ -= i;
```

`i` 是已经成功投递的数量。失败的第 `i` 条也要保留，所以仍然减 `i`。
`i==0` 时源起点和目标起点相同，违反三参数 `std::move` 的前置条件；跳过无意义搬移即可。测试通过不代表这条标准前置条件可以忽略。

## 2. 断言：保留检查，去掉字面量的临时字符串

文件：[macros.h](../../common/macros.h)

```cpp
// 原来：调用前先把错误提示转换成 std::string。
ASSERT(bool cond, const std::string &msg)

// 现在：借用错误提示，不复制文本。
ASSERT(bool cond, std::string_view msg)
```

条件仍会执行，失败仍打印错误并退出。没有把 Release 的检查编译掉。动态拼接的消息依然会提前求值，因此不能说所有断言都已经零分配。

## 3. 消费循环：peek 已经完成非空判断

涉及 [market_data_publisher.cpp](../../exchange/market_data/market_data_publisher.cpp)、[snapshot_synthesizer.cpp](../../exchange/market_data/snapshot_synthesizer.cpp)、[order_server.h](../../exchange/order_server/order_server.h)。

```cpp
// 原来
for (auto p = queue->peek(); queue->size() && p; p = queue->peek()) {
    // 原有业务处理
}

// 现在
for (auto p = queue->peek(); p; p = queue->peek()) {
    // 原有业务处理
}
```

这些调用点只有一个消费者；非空的借用指针在 `pop()` 前不会被生产者覆写。
原来的发送失败 `break`、成功后 `pop`、递增序号等处理都保留。停机统计中的 `size()` 没有删掉。

## 4. 内存池：空闲索引缩为 32 位

文件：[mem_pool.h](../../common/mem_pool.h)

```cpp
// 原来
std::vector<size_t> free_;

// 现在
using Index = uint32_t;
std::vector<Index> free_;
```

构造时先校验容量在 `[1, UINT32_MAX]`，再分配，写入索引时显式转换。当前业务池容量为 1,048,576 或更小，全部符合新约束。

对象存储和 `used` 标记仍保留，地址不因分配/释放而移动。仍然先构造成功，再弹出空闲索引，避免构造异常丢槽；归还时仍检查指针范围、槽位边界和重复释放，并调用对象析构。

没有拆分占用标记或加入共享无锁空闲链表。本次以可验证的内存节省为目标，不把布局更复杂等同于速度更快。

## 5. 队列：先验证容量，再使用掩码

文件：[lf_queue.h](../../common/lf_queue.h)

```cpp
// 原来
store_[tail % store_.size()] = value;

// 现在
store_[tail & mask_] = value;
```

生产、消费、peek 和批量写入共四处索引都改为掩码。构造时：

```cpp
if (!std::has_single_bit(capacity) || capacity > SIZE_MAX / 2)
    throw std::invalid_argument(...);
```

校验放在 `vector` 分配之前。`mask_ = capacity - 1`，游标仍在 `2*capacity` 回绕；满/空判断、全部槽位可用的约定、release/acquire 内存序不变。

大缓冲继续使用堆上的 `vector`，没有改成栈上内嵌数组。现有示例容量从 20 改成 32，测试中 3 槽改成 4 槽。

恢复测试不只是把 74 改成 128：128 槽先占 54 槽，只剩 74 槽，75 条完整恢复事件必须全部拒绝；清空后整批成功，再验证超容量批次、恰好满容量和跨尾部回绕。

## 6. 注释：保留机制，删除重复和错误推断

网络代码采用“文件开头说明关键机制，代码行旁一句解释”。保留非阻塞连接的 `SO_ERROR`、TCP 短写/残帧/背压、组播接口、截断检测和快照校验。

修正的主要误解：

- 游标会回绕，不是永不归零。
- 主线程等的是原子 `quiesced_`，没有直接并发读取定序器的普通计数。
- UDP 进入内核发送缓冲不表示订阅方收到；仍需序号与恢复机制。
- 组播 `send()` 在空间不足时会尝试刷新，不能说它绝不执行系统调用。
- `TRADE` 通知成交，`MODIFY/CANCEL` 更新订单簿，当前处理不需要重复扣量。
- 不保证某个操作固定节省多少周期，也不把缓存失效描述为必然访问主存。
