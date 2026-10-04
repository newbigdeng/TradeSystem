# 01 源码对照

[返回主报告](01_reliability.md)。点击具体问题展开代码。

片段取自实际 Git 版本或已核实的工作区，标注文件、版本及行号。片段省略上下文；完整实现见对应版本链接。新增功能注明原来没有该实现，未保存的旧辅助源码注明缺失。

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

<details>
<summary>01｜队列满时拒绝写入，保留已有消息</summary>

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

</details>

<a id="source-02"></a>

<details>
<summary>02｜生产者处理下游队列满的结果</summary>

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

</details>

<a id="source-03"></a>

<details>
<summary>03｜Logger 以整条记录入队，串行化多写入者</summary>

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

</details>

<a id="source-04"></a>

<details>
<summary>04｜TCP 部分发送保留未发送尾部</summary>

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

</details>

<a id="source-05"></a>

<details>
<summary>05｜整帧成功进入发送缓冲才推进订单序号</summary>

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

</details>

<a id="source-06"></a>

<details>
<summary>06｜TCP 区分 EOF，并继续处理背压留下的帧</summary>

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

</details>

<a id="source-07"></a>

<details>
<summary>07｜非阻塞连接检查真实完成结果</summary>

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

</details>

<a id="source-08"></a>

<details>
<summary>08｜内存池完整使用容量，并真正析构对象</summary>

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

</details>

<a id="source-09"></a>

<details>
<summary>09｜相同接收时间戳保持原到达顺序</summary>

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


**为什么这样改，以及如何验证：** 继续按时间比较，相同时间保留收集顺序。见 [perf_fifo_before.json](perf_fifo_before.json) / [perf_fifo_after.json](perf_fifo_after.json)；该轮幅度小，不认定显著提速。

</details>

<a id="source-10"></a>

<details>
<summary>10｜用完整方向和价格标识价位</summary>

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

</details>

<a id="source-11"></a>

<details>
<summary>11｜稀疏订单索引与修改前输入校验</summary>

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

</details>

<a id="source-12"></a>

<details>
<summary>12｜明确订单字节协议与会话身份</summary>

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

</details>

<a id="source-13"></a>

<details>
<summary>13｜撤单拒绝恢复订单状态，迟到确认不复活订单</summary>

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

</details>

<a id="source-14"></a>

<details>
<summary>14｜所有新单经过最终风控门，并预占待成交风险</summary>

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

</details>

<a id="source-15"></a>

<details>
<summary>15｜先去重回报，再更新持仓；不错误释放原订单风险</summary>

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

</details>

<a id="source-16"></a>

<details>
<summary>16｜快照需要全品种清空、同周期和状态哈希</summary>

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

</details>

<a id="source-17"></a>

<details>
<summary>17｜恢复批次整体发布，下游再核验簿</summary>

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

</details>

<a id="source-18"></a>

<details>
<summary>18｜线程持有任务和句柄，停止时真正 join</summary>

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

</details>

<a id="source-19"></a>

<details>
<summary>19｜信号处理只置标记，按依赖顺序排空</summary>

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

</details>

<a id="source-20"></a>

<details>
<summary>20｜特征按品种保存，先检查有效盘口</summary>

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

</details>

<a id="source-21"></a>

<details>
<summary>21｜关键交易审计独立于可丢诊断日志</summary>

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

</details>

<a id="source-22"></a>

<details>
<summary>22｜格式化时间使用调用内缓冲，移除共享成员写入</summary>

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

</details>

<a id="source-23"></a>

<details>
<summary>23｜客户端行情簿校验变更，CLEAR 同时释放并重置盘口</summary>

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

</details>

<a id="source-24"></a>

<details>
<summary>24｜UDP 保留未发送报文，并识别截断</summary>

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

</details>

<a id="source-25"></a>

<details>
<summary>25｜盘口汇总数量使用更宽类型</summary>

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

</details>

<a id="source-26"></a>

<details>
<summary>26｜进入撮合和发布回报前分别登记审计</summary>

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

</details>
