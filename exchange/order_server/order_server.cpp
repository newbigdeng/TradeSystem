#include "order_server.h"

namespace Exchange {
  OrderServer::OrderServer(ClientRequestLFQueue *client_requests, ClientResponseLFQueue *client_responses, const std::string &iface,int port,Common::CriticalJournal* audit)
      : iface_(iface), port_(port), outgoing_responses_(client_responses), logger_("exchange_order_server.log"),
        tcp_server_(logger_), fifo_sequencer_(client_requests,&logger_),audit_(audit) {
    cid_next_outgoing_seq_num_.fill(1);
    cid_next_exp_seq_num_.fill(1);
    cid_tcp_socket_.fill(nullptr);

    tcp_server_.recv_callback_ = [this](auto socket, auto rx_time) { recvCallback(socket, rx_time); };
    tcp_server_.recv_finished_callback_ = [this]() { recvFinishedCallback(); };
  }

  OrderServer::~OrderServer() {
    stop();

}

  /// Start and stop the order server main thread.
  auto OrderServer::start() -> void {
    run_ = true;
    tcp_server_.listen(iface_, port_);

    worker_.reset(Common::createAndStartThread(-1, "Exchange/OrderServer", [this]() { run(); }));
  }

  auto OrderServer::stop() -> void {
    accepting_.store(false);stop_deadline_=Common::getMonotonicNanos()+5*NANOS_TO_SECS;run_=false;
    if(worker_ && worker_->joinable())worker_->join();
  }
}
