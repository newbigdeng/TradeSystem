# 07｜发送推进修改前后

本轮最终修改只涉及发送推进及对应回归。容量限制、空闲睡眠、空批次抑制等候选未进入最终实现。这里的代码为聚焦差异的摘录；完整补丁见 [final.patch](evidence/final.patch)。

## 1. TCPSocket：分出只发送的入口

文件：`common/tcp_socket.h`、`common/tcp_socket.cpp`。

修改前，发送和接收在同一个入口：

```cpp
bool TCPSocket::sendAndRecv() noexcept {
    // 原有连接状态检查与最多 8 次非阻塞 send。
    // 原有 recvmsg 与接收回调。
}
```

修改后，把原发送部分原样交给 `flushSend()`，旧入口仍按原顺序调用：

```cpp
bool TCPSocket::flushSend() noexcept {
    // 原有状态检查、短写偏移、EINTR、EAGAIN 和错误处理。
    return healthy();
}
bool TCPSocket::sendAndRecv() noexcept {
    if (!flushSend()) return false;
    // 原有 recvmsg 与接收回调。
}
```

EAGAIN 只表示这轮不能继续写，尾部留在缓冲；返回健康不代表所有字节都发完。是否排空仍看 pending_bytes。

## 2. TCPServer：刷新所有有回报的连接，不夹杂接收

文件：`common/tcp_server.h`。新增：

```cpp
void flushSend() noexcept {
    for (auto* socket : receive_sockets_)
        if (socket->pending_bytes()) socket->flushSend();
}
```

不能用 `sendAndRecv()` 代替这一步，否则会重新进入前面连接的业务接收回调。

## 3. OrderServer：本轮入缓冲，本轮就尝试发送

文件：`exchange/order_server/order_server.h`。

修改前的循环顺序：

```cpp
tcp_server_.sendAndRecv();
for (auto* response = outgoing_responses_->peek(); response;
     response = outgoing_responses_->peek()) {
    // 完整帧进入连接的发送缓冲后，pop 并推进序号。
}
// 下一轮才开始真正发送刚才的回报。
```

修改后：

```cpp
tcp_server_.sendAndRecv();
bool queued_response = false;
for (auto* response = outgoing_responses_->peek(); response;
     response = outgoing_responses_->peek()) {
    // Full 时保留队首；Accepted 后设置标志，再 pop 并推进序号。
    queued_response = true;
}
if (queued_response) tcp_server_.flushSend();
```

上面省略了编码、身份检查与错误处理，不是完整可替换函数。完整生产代码保留逐行网络注释。

这解决的是“已加入用户态缓冲的回报还要等下一轮发送”的顺序问题，不能保证内核可写，也没有解决回报尚在队列时被长接收批次拖延的问题。

## 4. 回归与未采用实验

`network_progress_test.cpp` 验证短写、EAGAIN、断开对端、纯发送不读输入，以及跨连接发送推进。它接入 CTest 的 `network_flush_progress`。

未采用候选在 `experiments/` 留档，不参与默认构建。候选的阶段测试通过只说明所覆盖的正确性条件通过，不等于性能符合要求。详情见主报告。
