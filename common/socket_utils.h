// IPv4 socket 工具。TCP 是字节流，UDP 保留数据报边界。
// 非阻塞 connect 返回 EINPROGRESS 后，要等可写并检查 SO_ERROR，不能只凭 poll 成功判断连接成功。
// 组播发送通过 IP_MULTICAST_IF 选择出口；接收通过 IP_ADD_MEMBERSHIP 指定组和接口。
// 当前接口名解析采用找到的首个 IPv4 地址；TCP 客户端传入 iface 不等于绑定源接口。

#pragma once
#include <cerrno>
#include <iostream>
#include <string>
#include <unordered_set>
#include <sstream>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <poll.h>
#include "macros.h"
#include "logging.h"

namespace Common {
struct SocketCfg {
  std::string ip_;                   // 目标 IPv4 文本；为空时用接口地址。
  std::string iface_;                // 网卡名称，用于查 IPv4 和选择组播接口。
  int port_ = -1;                    // 主机字节序端口。
  bool is_udp_ = false;              // 选择 UDP 或 TCP。
  bool is_listening_ = false;        // 监听/接收端走 bind，主动发送端走 connect。
  bool needs_so_timestamp_ = false;  // 是否请求内核接收时间戳。

  // 生成诊断文本，不作为网络编码。
  auto toString() const {
    std::stringstream ss;
    ss << "SocketCfg[ip:" << ip_ << " iface:" << iface_ << " port:" << port_
       << " is_udp:" << is_udp_ << " is_listening:" << is_listening_
       << " needs_SO_timestamp:" << needs_so_timestamp_ << "]";
    return ss.str();
  }
};

constexpr int MaxTCPServerBacklog = 1024;  // listen 接入队列的请求上限。

// 查找指定网卡的首个 IPv4 地址。
inline auto getIfaceIP(const std::string& iface) -> std::string {
  char buf[NI_MAXHOST] = {'\0'};                             // 准备清零的数字地址缓冲。
  ifaddrs* ifaddr = nullptr;                                 // 接收接口链表的首指针。
  if (getifaddrs(&ifaddr) != -1) {                           // 枚举系统接口；失败时返回空字符串。
    for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {  // 遍历接口地址记录。
      // 跳过空地址、非 IPv4 和名称不匹配项。
      if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET && iface == ifa->ifa_name) {
        // 转成数字 IPv4 文本，不做反向 DNS。
        getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), buf, sizeof(buf), NULL, 0, NI_NUMERICHOST);
        break;  // 采用首个匹配地址。
      }
    }

    freeifaddrs(ifaddr);  // 归还系统分配的接口链表。
  }

  return buf;  // 返回查得的地址；未找到时为空。
}

// 保留已有文件标志并开启非阻塞。
inline auto setNonBlocking(int fd) -> bool {
  const auto flags = fcntl(fd, F_GETFL, 0);  // 读取 fd 当前标志。
  if (flags == -1)
    return false;          // 读取失败则返回失败。
  if (flags & O_NONBLOCK)  // 已经非阻塞时无需重复设置。
    return true;
  return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1);  // 保留原标志并添加 O_NONBLOCK。
}

// 关闭 TCP Nagle，减少小包等待。
inline auto disableNagle(int fd) -> bool {
  int one = 1;  // 套接字选项的启用值。

  // 设置 TCP_NODELAY，返回是否成功。
  return (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<void*>(&one), sizeof(one)) !=
          -1);
}

// 请求内核提供接收时间戳。
inline auto setSOTimestamp(int fd) -> bool {
  int one = 1;  // 套接字选项的启用值。

  // 启用 SO_TIMESTAMP，时间戳在 recvmsg 控制消息中读取。
  return (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, reinterpret_cast<void*>(&one), sizeof(one)) !=
          -1);
}

// 在指定 IPv4 接口加入组播组。
inline auto join(int fd, const std::string& ip, const std::string& iface) -> bool {
  const auto iface_ip = getIfaceIP(iface);  // 把接口名解析为本机 IPv4。
  if (iface_ip.empty()) {                   // 找不到该接口地址。
    errno = ENODEV;                         // 设置无设备错误码。
    return false;                           // 返回加入失败。
  }

  ip_mreq mreq{};                                                  // 清零组播成员配置。
  if (inet_pton(AF_INET, ip.c_str(), &mreq.imr_multiaddr) != 1 ||  // 解析组播目的地址。
      // 确认地址属于 IPv4 组播范围。
      !IN_MULTICAST(ntohl(mreq.imr_multiaddr.s_addr))) {
    errno = EINVAL;  // 无效参数错误码。
    return false;    // 返回加入失败。
  }

  inet_pton(AF_INET, iface_ip.c_str(), &mreq.imr_interface);  // 填写接收组播的本地接口地址。

  // 让内核在指定接口加入该组。
  return (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != -1);
}

// 创建、配置并连接或绑定 socket，调用方必须检查返回值。
[[nodiscard]] inline auto createSocket(Logger& logger, const SocketCfg& socket_cfg) -> int {
  std::string time_str;  // 日志时间字符串缓冲。

  // 优先用指定 IP，否则查接口 IP。
  const auto ip = socket_cfg.ip_.empty() ? getIfaceIP(socket_cfg.iface_) : socket_cfg.ip_;
  logger.log("%:% %() % cfg:%\n", __FILE__, __LINE__, __FUNCTION__,
             // 补充日志时间和相关字段。
             Common::getCurrentTimeStr(&time_str), socket_cfg.toString());

  // 地址和端口均为数字；监听端增加 AI_PASSIVE。
  const int input_flags =
      (socket_cfg.is_listening_ ? AI_PASSIVE : 0) | (AI_NUMERICHOST | AI_NUMERICSERV);

  // 限定 IPv4，并选择数据报或字节流。
  const addrinfo hints{input_flags, AF_INET, socket_cfg.is_udp_ ? SOCK_DGRAM : SOCK_STREAM,
                       // 选择 UDP/TCP 协议，其余查询字段清零。
                       socket_cfg.is_udp_ ? IPPROTO_UDP : IPPROTO_TCP, 0, 0, nullptr, nullptr};
  addrinfo* result = nullptr;  // 地址解析结果链表。

  // 把地址与端口转换为 sockaddr 候选。
  const auto rc =
      getaddrinfo(ip.c_str(), std::to_string(socket_cfg.port_).c_str(), &hints, &result);

  // 地址解析失败时退出并记录原因。
  ASSERT(!rc, "getaddrinfo() failed. error:" + std::string(gai_strerror(rc)) +
                  "errno:" + strerror(errno));
  int socket_fd = -1;  // 初始为无效 fd。
  int one = 1;         // 套接字选项的启用值。

  // 遍历地址候选；当前数字 IPv4 查询通常返回一项。
  for (addrinfo* rp = result; rp; rp = rp->ai_next) {
    // 创建内核 socket；失败立即退出。
    ASSERT((socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) != -1,
           "socket() failed. errno:" + std::string(strerror(errno)));

    // 设置非阻塞，避免 I/O 阻塞工作线程。
    ASSERT(setNonBlocking(socket_fd),
           "setNonBlocking() failed. errno:" + std::string(strerror(errno)));
    if (!socket_cfg.is_udp_) {  // 仅 TCP 需要关闭 Nagle。
      // 设置 TCP_NODELAY 并检查结果。
      ASSERT(disableNagle(socket_fd),
             "disableNagle() failed. errno:" + std::string(strerror(errno)));
    }

    // 按已限定的 IPv4 格式访问目的地址。
    const auto* ipv4_addr = reinterpret_cast<const sockaddr_in*>(rp->ai_addr);
    if (socket_cfg.is_udp_ && !socket_cfg.is_listening_ &&  // 仅为 UDP 主动发送端配置组播出口。
        // 进一步确认目标是组播地址。
        IN_MULTICAST(ntohl(ipv4_addr->sin_addr.s_addr))) {
      const auto iface_ip = getIfaceIP(socket_cfg.iface_);  // 查出所选接口的本机 IPv4。

      // 接口不存在时明确失败。
      ASSERT(!iface_ip.empty(), "No IPv4 address for multicast interface: " + socket_cfg.iface_);
      in_addr iface_addr{};                                           // 初始化出口地址结构。
      ASSERT(inet_pton(AF_INET, iface_ip.c_str(), &iface_addr) == 1,  // 把出口地址转成网络格式。
             // 出口地址无法解析时的错误说明。
             "Invalid multicast interface address: " + iface_ip);

      // 在 connect 前选定组播出口接口。
      ASSERT(
          setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) == 0,
          // 设置出口失败时的错误说明。
          "setsockopt() IP_MULTICAST_IF failed. errno:" + std::string(strerror(errno)));
    }

    if (!socket_cfg.is_listening_) {  // 主动端设置远端地址。
      // TCP 发起连接；UDP connect 只设置默认对端，不握手。
      const auto connected = ::connect(socket_fd, rp->ai_addr, rp->ai_addrlen);
      if (connected < 0) {           // 处理连接未立即成功的情况。
        int error = errno;           // 保存当前错误码。
        if (error == EINPROGRESS) {  // 非阻塞 TCP 仍在建立连接。
          pollfd pending{socket_fd, POLLOUT, 0};
          int ready;  // 准备等待可写事件。

          // 每次最多等 5 秒，被信号中断则重试。
          do {
            ready = ::poll(&pending, 1, 5000);
          } while (ready < 0 && errno == EINTR);
          socklen_t size = sizeof(error);  // 准备 SO_ERROR 返回值长度。

          // 可写后读取最终连接结果；error 为 0 才成功。
          if (ready > 0 && getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0) {
          } else
            error = ready == 0 ? ETIMEDOUT : errno;  // 区分超时和 poll/getsockopt 失败。
        }

        // 失败时关闭 fd、释放地址链表并保留错误码。
        if (error) {
          ::close(socket_fd);
          socket_fd = -1;
          freeaddrinfo(result);
          errno = error;
          return -1;
        }
      }
    }

    if (socket_cfg.is_listening_) {  // 监听端允许重用绑定地址。
      // 设置 SO_REUSEADDR，不等同于 SO_REUSEPORT。
      ASSERT(setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                        sizeof(one)) == 0,
             "setsockopt() SO_REUSEADDR failed. errno:" + std::string(strerror(errno)));
    }

    if (socket_cfg.is_listening_) {  // 监听/接收端绑定本机端口。
      // UDP 接收绑定通配地址；端口转网络字节序。
      const sockaddr_in addr{AF_INET, htons(socket_cfg.port_), {htonl(INADDR_ANY)}, {}};

      // UDP 用通配地址，TCP 用解析所得地址绑定。
      ASSERT(
          bind(socket_fd,
               socket_cfg.is_udp_ ? reinterpret_cast<const struct sockaddr*>(&addr) : rp->ai_addr,
               sizeof(addr)) == 0,
          "bind() failed. errno:%" + std::string(strerror(errno)));
    }

    if (!socket_cfg.is_udp_ && socket_cfg.is_listening_) {  // 只有 TCP 监听端需要 listen。
      // 把 socket 转为监听状态并检查结果。
      ASSERT(listen(socket_fd, MaxTCPServerBacklog) == 0,
             "listen() failed. errno:" + std::string(strerror(errno)));
    }

    if (socket_cfg.needs_so_timestamp_) {  // 按配置决定是否启用接收时间戳。
      // 检查时间戳选项设置结果。
      ASSERT(setSOTimestamp(socket_fd),
             "setSOTimestamp() failed. errno:" + std::string(strerror(errno)));
    }
  }

  freeaddrinfo(result);  // 释放地址解析结果。
  return socket_fd;      // 返回已经配置好的 fd。
}
}
