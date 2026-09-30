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
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>

#include "macros.h"

#include "logging.h"

namespace Common {
  struct SocketCfg {
    std::string ip_;
    std::string iface_;
    int port_ = -1;
    bool is_udp_ = false;
    bool is_listening_ = false;
    bool needs_so_timestamp_ =  false;

    auto toString() const {
      std::stringstream ss;
      ss << "SocketCfg[ip:" << ip_
      << " iface:" << iface_
      << " port:" << port_
      << " is_udp:" << is_udp_
      << " is_listening:" << is_listening_
      << " needs_SO_timestamp:" << needs_so_timestamp_
      << "]";

      return ss.str();
    }
  };

  /// Represents the maximum number of pending / unaccepted TCP connections.
  constexpr int MaxTCPServerBacklog = 1024;

  /// Convert interface name "eth0" to ip "123.123.123.123".
  inline auto getIfaceIP(const std::string &iface) -> std::string {
    char buf[NI_MAXHOST] = {'\0'};
    ifaddrs *ifaddr = nullptr;

    if (getifaddrs(&ifaddr) != -1) {
      for (ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET && iface == ifa->ifa_name) {
          getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), buf, sizeof(buf), NULL, 0, NI_NUMERICHOST);
          break;
        }
      }
      freeifaddrs(ifaddr);
    }

    return buf;
  }

  /// Sockets will not block on read, but instead return immediately if data is not available.
  inline auto setNonBlocking(int fd) -> bool {
    const auto flags = fcntl(fd, F_GETFL, 0);
    if(flags==-1) return false;
    if (flags & O_NONBLOCK)
      return true;
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1);
  }

  /// Disable Nagle's algorithm and associated delays.
  inline auto disableNagle(int fd) -> bool {
    int one = 1;
    return (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<void *>(&one), sizeof(one)) != -1);
  }

  /// Allow software receive timestamps on incoming packets.
  inline auto setSOTimestamp(int fd) -> bool {
    int one = 1;
    return (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, reinterpret_cast<void *>(&one), sizeof(one)) != -1);
  }

  /// Add / Join membership / subscription to the multicast stream specified and on the interface specified.
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
  [[nodiscard]] inline auto createSocket(Logger &logger, const SocketCfg& socket_cfg) -> int {
    std::string time_str;

    const auto ip = socket_cfg.ip_.empty() ? getIfaceIP(socket_cfg.iface_) : socket_cfg.ip_;
    logger.log("%:% %() % cfg:%\n", __FILE__, __LINE__, __FUNCTION__,
               Common::getCurrentTimeStr(&time_str), socket_cfg.toString());

    const int input_flags = (socket_cfg.is_listening_ ? AI_PASSIVE : 0) | (AI_NUMERICHOST | AI_NUMERICSERV);
    const addrinfo hints{input_flags, AF_INET, socket_cfg.is_udp_ ? SOCK_DGRAM : SOCK_STREAM,
                         socket_cfg.is_udp_ ? IPPROTO_UDP : IPPROTO_TCP, 0, 0, nullptr, nullptr};

    addrinfo *result = nullptr;
    const auto rc = getaddrinfo(ip.c_str(), std::to_string(socket_cfg.port_).c_str(), &hints, &result);
    ASSERT(!rc, "getaddrinfo() failed. error:" + std::string(gai_strerror(rc)) + "errno:" + strerror(errno));

    int socket_fd = -1;
    int one = 1;
    for (addrinfo *rp = result; rp; rp = rp->ai_next) {
      ASSERT((socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) != -1, "socket() failed. errno:" + std::string(strerror(errno)));

      ASSERT(setNonBlocking(socket_fd), "setNonBlocking() failed. errno:" + std::string(strerror(errno)));

      if (!socket_cfg.is_udp_) { // disable Nagle for TCP sockets.
        ASSERT(disableNagle(socket_fd), "disableNagle() failed. errno:" + std::string(strerror(errno)));
      }

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
      }

      if (socket_cfg.is_listening_) { // allow re-using the address in the call to bind()
        ASSERT(setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one)) == 0, "setsockopt() SO_REUSEADDR failed. errno:" + std::string(strerror(errno)));
      }

      if (socket_cfg.is_listening_) {
        // bind to the specified port number.
        const sockaddr_in addr{AF_INET, htons(socket_cfg.port_), {htonl(INADDR_ANY)}, {}};
        ASSERT(bind(socket_fd, socket_cfg.is_udp_ ? reinterpret_cast<const struct sockaddr *>(&addr) : rp->ai_addr, sizeof(addr)) == 0, "bind() failed. errno:%" + std::string(strerror(errno)));
      }

      if (!socket_cfg.is_udp_ && socket_cfg.is_listening_) { // listen for incoming TCP connections.
        ASSERT(listen(socket_fd, MaxTCPServerBacklog) == 0, "listen() failed. errno:" + std::string(strerror(errno)));
      }

      if (socket_cfg.needs_so_timestamp_) { // enable software receive timestamps.
        ASSERT(setSOTimestamp(socket_fd), "setSOTimestamp() failed. errno:" + std::string(strerror(errno)));
      }
    }

    freeaddrinfo(result);
    return socket_fd;
  }
}
