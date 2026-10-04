# 00 源码对照

[返回主报告](00_multicast_interface.md)。点击具体问题展开代码。

片段取自实际 Git 版本或已核实的工作区，标注文件、版本及行号。片段省略上下文；完整实现见对应版本链接。新增功能注明原来没有该实现，未保存的旧辅助源码注明缺失。

| 对照 | 内容 |
|---|---|
| 01 | [发送端把配置接口交给内核](#source-01) |
| 02 | [接收端在指定接口入组](#source-02) |
| 03 | [封装保存并继续传递接口](#source-03) |

<a id="source-01"></a>

<details>
<summary>01｜发送端把配置接口交给内核</summary>

组播目标 IP 说明接收组，并不能代替本地网卡选择。旧代码确定目标地址后直接连接，没有设置组播发送接口。

**修改前**

[`common/socket_utils.h`，`ff20fd7`，第 97–99 行](https://github.com/newbigdeng/TradeSystem/blob/ff20fd7a4c3ff0af78bd41cb8c46f0fe008485ef/common/socket_utils.h#L97)

```cpp
    const auto ip = socket_cfg.ip_.empty() ? getIfaceIP(socket_cfg.iface_) : socket_cfg.ip_;
    logger.log("%:% %() % cfg:%\n", __FILE__, __LINE__, __FUNCTION__,
               Common::getCurrentTimeStr(&time_str), socket_cfg.toString());
```


**修改后**

[`common/socket_utils.h`，`e2ea493`，第 132–148 行](https://github.com/newbigdeng/TradeSystem/blob/e2ea4935831558706069baf9440970e546a3bd7c/common/socket_utils.h#L132)

```cpp
      // Multicast destination addresses do not select the local interface.
      // Set the configured IPv4 interface before connect() chooses a route.
      const auto *ipv4_addr = reinterpret_cast<const sockaddr_in *>(rp->ai_addr);
      if (socket_cfg.is_udp_ && !socket_cfg.is_listening_ &&
          IN_MULTICAST(ntohl(ipv4_addr->sin_addr.s_addr))) {
        const auto iface_ip = getIfaceIP(socket_cfg.iface_);
        ASSERT(!iface_ip.empty(), "No IPv4 address for multicast interface: " + socket_cfg.iface_);
        in_addr iface_addr{};
        ASSERT(inet_pton(AF_INET, iface_ip.c_str(), &iface_addr) == 1,
               "Invalid multicast interface address: " + iface_ip);
        ASSERT(setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) == 0,
               "setsockopt() IP_MULTICAST_IF failed. errno:" + std::string(strerror(errno)));
      }

      if (!socket_cfg.is_listening_) { // establish connection to specified address.
        ASSERT(connect(socket_fd, rp->ai_addr, rp->ai_addrlen) != 1, "connect() failed. errno:" + std::string(strerror(errno)));
      }
```


**为什么这样改，以及如何验证：** 增加的条件只覆盖 UDP 组播发送端；查找配置接口的 IPv4 地址并在 connect 前设置 IP_MULTICAST_IF。原目标地址表达式仍用于解析目标，并未被错误地改成本地地址。对应真实 lo 收发测试见 [multicast_interface_test.cpp](multicast_interface_test.cpp)；主报告“重新测试的结果”保留历史端到端结果。

</details>

<a id="source-02"></a>

<details>
<summary>02｜接收端在指定接口入组</summary>

原 imr_interface 为 INADDR_ANY，没有传入配置中的 lo；修改后完整校验组地址与接口，再执行入组。

**修改前**

[`common/socket_utils.h`，`ff20fd7`，第 88–91 行](https://github.com/newbigdeng/TradeSystem/blob/ff20fd7a4c3ff0af78bd41cb8c46f0fe008485ef/common/socket_utils.h#L88)

```cpp
  inline auto join(int fd, const std::string &ip) -> bool {
    const ip_mreq mreq{{inet_addr(ip.c_str())}, {htonl(INADDR_ANY)}};
    return (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != -1);
  }
```


**修改后**

[`common/socket_utils.h`，`e2ea493`，第 89–105 行](https://github.com/newbigdeng/TradeSystem/blob/e2ea4935831558706069baf9440970e546a3bd7c/common/socket_utils.h#L89)

```cpp
  inline auto join(int fd, const std::string &ip, const std::string &iface) -> bool {
    const auto iface_ip = getIfaceIP(iface);
    if (iface_ip.empty()) {
      errno = ENODEV;
      return false;
    }
    ip_mreq mreq{};
    if (inet_pton(AF_INET, ip.c_str(), &mreq.imr_multiaddr) != 1 ||
        !IN_MULTICAST(ntohl(mreq.imr_multiaddr.s_addr))) {
      errno = EINVAL;
      return false;
    }
    inet_pton(AF_INET, iface_ip.c_str(), &mreq.imr_interface);
    return (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != -1);
  }

  /// Create a TCP / UDP socket to either connect to or listen for data on or listen for connections on the specified interface and IP:port information.
```


**为什么这样改，以及如何验证：** 接口没有 IPv4 地址时返回 ENODEV，目标不是合法 IPv4 组播地址时返回 EINVAL；成功路径把接口地址写入 imr_interface。回归覆盖增量组、快照组、无效接口及非组播地址。

</details>

<a id="source-03"></a>

<details>
<summary>03｜封装保存并继续传递接口</summary>

工具函数需要接口参数，因此 McastSocket 的初始化和入组必须形成完整传递链。

**修改前**

[`common/mcast_socket.cpp`，`ff20fd7`，第 6–15 行](https://github.com/newbigdeng/TradeSystem/blob/ff20fd7a4c3ff0af78bd41cb8c46f0fe008485ef/common/mcast_socket.cpp#L6)

```cpp
  auto McastSocket::init(const std::string &ip, const std::string &iface, int port, bool is_listening) -> int {
    const SocketCfg socket_cfg{ip, iface, port, true, is_listening, false};
    socket_fd_ = createSocket(logger_, socket_cfg);
    return socket_fd_;
  }

  /// Add / Join membership / subscription to a multicast stream.
  bool McastSocket::join(const std::string &ip) {
    return Common::join(socket_fd_, ip);
  }
```


**修改后**

[`common/mcast_socket.cpp`，`e2ea493`，第 6–16 行](https://github.com/newbigdeng/TradeSystem/blob/e2ea4935831558706069baf9440970e546a3bd7c/common/mcast_socket.cpp#L6)

```cpp
  auto McastSocket::init(const std::string &ip, const std::string &iface, int port, bool is_listening) -> int {
    iface_ = iface;
    const SocketCfg socket_cfg{ip, iface, port, true, is_listening, false};
    socket_fd_ = createSocket(logger_, socket_cfg);
    return socket_fd_;
  }

  /// Add / Join membership / subscription to a multicast stream.
  bool McastSocket::join(const std::string &ip) {
    return Common::join(socket_fd_, ip, iface_);
  }
```

[`common/mcast_socket.h`，`e2ea493`，第 38–39 行](https://github.com/newbigdeng/TradeSystem/blob/e2ea4935831558706069baf9440970e546a3bd7c/common/mcast_socket.h#L38)

```cpp
    /// Preserve the configured interface for initial and snapshot subscriptions.
    std::string iface_;
```


**为什么这样改，以及如何验证：** init 保存 iface，join 使用保存值。初次增量订阅与后续快照订阅因此遵循同一配置。新增字段是补齐传递链，不是改系统路由；UFW 保持开启。

</details>
