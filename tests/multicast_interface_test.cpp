#include <chrono>
#include <stdexcept>
#include <thread>

#include "common/mcast_socket.h"

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

void checkStream(Common::Logger &logger, const std::string &group) {
  Common::McastSocket receiver(logger);
  receiver.init(group, "lo", 0, true);
  require(receiver.join(group), "could not join multicast on lo");
  require(!Common::join(receiver.socket_fd_, group, "no-such-iface") && errno == ENODEV,
          "unknown interface must fail instead of using the default route");
  require(!Common::join(receiver.socket_fd_, "127.0.0.1", "lo") && errno == EINVAL,
          "non-multicast group must be rejected");

  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  require(getsockname(receiver.socket_fd_, reinterpret_cast<sockaddr *>(&bound), &bound_len) == 0,
          "getsockname failed");
  Common::McastSocket sender(logger);
  sender.init(group, "lo", ntohs(bound.sin_port), false);
  in_addr selected{};
  socklen_t selected_len = sizeof(selected);
  require(getsockopt(sender.socket_fd_, IPPROTO_IP, IP_MULTICAST_IF, &selected, &selected_len) == 0,
          "cannot inspect selected multicast interface");
  require(selected.s_addr == inet_addr("127.0.0.1"), "sender did not select lo");

  bool received = false;
  const std::string payload = "multicast-interface-regression";
  receiver.recv_callback_ = [&](Common::McastSocket *socket) {
    received = socket->next_rcv_valid_index_ == payload.size() &&
               std::memcmp(socket->inbound_data_.data(), payload.data(), payload.size()) == 0;
    socket->next_rcv_valid_index_ = 0;
  };
  require(::send(sender.socket_fd_, payload.data(), payload.size(), 0) ==
          static_cast<ssize_t>(payload.size()), "multicast send failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!received && std::chrono::steady_clock::now() < deadline) {
    receiver.sendAndRecv();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  sender.leave(group, ntohs(bound.sin_port));
  receiver.leave(group, ntohs(bound.sin_port));
  require(received, "configured loopback multicast did not reach receiver");
  std::cout << "PASS " << group << " sender=127.0.0.1 receiver=lo payload verified\n";
}
} // namespace

int main(int argc, char **) {
  Common::Logger logger("multicast_interface_test.log");
  if (argc > 1) {
    Common::McastSocket invalid(logger);
    invalid.init("233.252.14.3", "no-such-iface", 20001, false);
    return 0; // The invalid interface must terminate initialization with failure.
  }
  try {
    checkStream(logger, "233.252.14.3");
    checkStream(logger, "233.252.14.1");
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
