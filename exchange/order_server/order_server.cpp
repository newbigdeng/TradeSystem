// 构造时初始化客户端序号和 socket 映射，并把网络回调绑定到当前对象。
// Logger 按成员声明顺序先于 TCPServer 构造；回调存储 this 指针，线程退出后才能析构。
// stop 不替代 quiesce；上层停机必须先排空已接受的暂存请求，再停止撮合与网络线程。

#include "order_server.h"

namespace Exchange {
// 保存队列、监听配置与可选审计对象。
OrderServer::OrderServer(ClientRequestLFQueue* client_requests,
                         ClientResponseLFQueue* client_responses, const std::string& iface,
                         int port, Common::CriticalJournal* audit, const std::string& listen_ip)
    // 先初始化配置、回报队列和日志。
    : iface_(iface),
      listen_ip_(listen_ip),
      port_(port),
      outgoing_responses_(client_responses),
      logger_("exchange_order_server.log"),
      // 向网络组件及定序器传入借用指针。
      tcp_server_(logger_),
      fifo_sequencer_(client_requests, &logger_),
      audit_(audit) {
  cid_next_outgoing_seq_num_.fill(1);  // 各客户出向回报序号从 1 开始。
  cid_next_exp_seq_num_.fill(1);       // 各客户期望请求序号从 1 开始。
  cid_tcp_socket_.fill(nullptr);       // 未绑定客户的 socket 必须是空指针。

  // 捕获 this 指针，将收包转交业务解析。
  tcp_server_.recv_callback_ = [this](auto socket, auto rx_time) { recvCallback(socket, rx_time); };

  // 轮末触发定序，无新收包时也会执行。
  tcp_server_.recv_finished_callback_ = [this]() { recvFinishedCallback(); };
}

// 析构前停止并等待网络线程。
OrderServer::~OrderServer() {
  stop();  // 调用幂等的停止逻辑。
}

// 开始监听并启动工作线程。
auto OrderServer::start() -> void {
  run_ = true;                                    // 发布运行标志。
  tcp_server_.listen(iface_, port_, listen_ip_);  // 把配置的监听地址传给 TCP socket。

  // 启动线程执行 run；-1 表示不指定绑核。
  worker_.reset(Common::createAndStartThread(-1, "Exchange/OrderServer", [this]() { run(); }));
}

// 停机第二阶段，排空输出并等待线程结束。
auto OrderServer::stop() -> void {
  // 禁止新业务并设置五秒输出排空期限。
  accepting_.store(false);
  stop_deadline_ = Common::getMonotonicNanos() + 5 * NANOS_TO_SECS;
  run_ = false;
  if (worker_ && worker_->joinable())
    worker_->join();  // 仅对可 join 的线程等待，避免重复 join。
}
}
