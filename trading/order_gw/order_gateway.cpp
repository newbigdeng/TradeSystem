#include "order_gateway.h"

namespace Trading {
  OrderGateway::OrderGateway(ClientId client_id,
                             Exchange::ClientRequestLFQueue *client_requests,
                             Exchange::ClientResponseLFQueue *client_responses,
                             std::string ip, const std::string &iface, int port)
      : client_id_(client_id), ip_(ip), iface_(iface), port_(port), outgoing_requests_(client_requests), incoming_responses_(client_responses),
      logger_("trading_order_gateway_" + std::to_string(client_id) + ".log"), tcp_socket_(logger_) {
    tcp_socket_.recv_callback_ = [this](auto socket, auto rx_time) { recvCallback(socket, rx_time); };
  }

  /// Main thread loop - sends out client requests to the exchange and reads and dispatches incoming client responses.
  auto OrderGateway::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr());
    while(run_ || outgoing_requests_->peek() || tcp_socket_.pending_bytes() || pending_requests_ || Common::getMonotonicNanos()-Nanos(last_receive_)<50*NANOS_TO_MILLIS) {
      if(!run_ && Common::getMonotonicNanos()>stop_deadline_.load()) {session_healthy_=false;logger_.log("ORDER SESSION UNKNOWN: shutdown deadline pending:% unsent_bytes:%\n",pending_requests_,tcp_socket_.pending_bytes());break;}
      tcp_socket_.sendAndRecv();
      if(!tcp_socket_.healthy() || !session_healthy_.load(std::memory_order_acquire)) {
        logger_.log("ORDER SESSION UNKNOWN: disconnect; stop new sends and reconcile before restart\n");
        session_healthy_.store(false,std::memory_order_release);run_=false; break;
      }

      for(auto client_request = outgoing_requests_->peek(); client_request; client_request = outgoing_requests_->peek()) {
        TTT_MEASURE(T11_OrderGateway_LFQueue_read, logger_);

        logger_.log("%:% %() % Sending cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(), client_id_, next_outgoing_seq_num_, client_request->toString());
        START_MEASURE(Trading_TCPSocket_send);
        const auto frame=Common::Wire::encode(Exchange::OMClientRequest{next_outgoing_seq_num_,*client_request,session_epoch_});
        const auto status=tcp_socket_.send(frame.data(),frame.size());
        if(status==Common::SendResult::Full) break;
        ASSERT(status==Common::SendResult::Accepted,"order session unknown: cannot queue complete frame");
        END_MEASURE(Trading_TCPSocket_send, logger_);
        outgoing_requests_->pop();
        TTT_MEASURE(T12_OrderGateway_TCP_write, logger_);

        next_outgoing_seq_num_++;++pending_requests_;
      }
    }
    logger_.log("GATEWAY STATS sent:% received_frames:% pending:% unsent_bytes:% healthy:% response_queue_full:%\n",next_outgoing_seq_num_-1,next_exp_seq_num_-1,pending_requests_,tcp_socket_.pending_bytes(),session_healthy_.load(),incoming_responses_->full_count());
  }

  /// Callback when an incoming client response is read, we perform some checks and forward it to the lock free queue connected to the trade engine.
  auto OrderGateway::recvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void {
    TTT_MEASURE(T7t_OrderGateway_TCP_read, logger_);

    START_MEASURE(Trading_OrderGateway_recvCallback);
    logger_.log("%:% %() % Received socket:% len:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(), socket->socket_fd_, socket->next_rcv_valid_index_, rx_time);

    size_t consumed=0;
    while(consumed+Common::Wire::ResponseSize<=socket->next_rcv_valid_index_) {
      Exchange::OMClientResponse response;
      if(!Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+consumed),response) || response.session_epoch_!=session_epoch_ || response.me_client_response_.client_id_!=client_id_) {
        session_healthy_.store(false,std::memory_order_release);socket->state_=Common::ConnectionState::Error;break;
      }
      if(response.seq_num_<next_exp_seq_num_) {consumed+=Common::Wire::ResponseSize;continue;} // already applied transport frame
      if(response.seq_num_!=next_exp_seq_num_) {session_healthy_.store(false,std::memory_order_release);socket->state_=Common::ConnectionState::Error;break;}
      const auto &r=response.me_client_response_;
      if(r.type_<Exchange::ClientResponseType::ACCEPTED || r.type_>Exchange::ClientResponseType::STATE ||
         (r.type_==Exchange::ClientResponseType::FILLED && (r.ticker_id_>=ME_MAX_TICKERS || (r.side_!=Side::BUY && r.side_!=Side::SELL) || !r.exec_qty_ || !r.response_id_ || r.exec_qty_>INT32_MAX || r.price_<=0 || r.price_==Price_INVALID))) {
        session_healthy_.store(false,std::memory_order_release);socket->state_=Common::ConnectionState::Error;break;
      }
      if(!incoming_responses_->try_push(r))break; // keep complete frame, retry after downstream drains
      if(r.reject_reason_==Exchange::RejectReason::IDENTITY || r.reject_reason_==Exchange::RejectReason::SEQUENCE || r.reject_reason_==Exchange::RejectReason::SESSION || r.reject_reason_==Exchange::RejectReason::VERSION)session_healthy_.store(false,std::memory_order_release);
      last_receive_=Common::getMonotonicNanos();
      if(r.type_!=Exchange::ClientResponseType::FILLED && pending_requests_)--pending_requests_;
      ++next_exp_seq_num_;consumed+=Common::Wire::ResponseSize;
    }
    if(consumed) {memmove(socket->inbound_data_.data(),socket->inbound_data_.data()+consumed,socket->next_rcv_valid_index_-consumed);socket->next_rcv_valid_index_-=consumed;}
    END_MEASURE(Trading_OrderGateway_recvCallback, logger_);
  }
}
