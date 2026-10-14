// 性能计时点由 TRADE_PERF_TRACE_EVERY 控制采样频率。
// 订单网关把请求编码为 TCP 帧，并把私有回报交给交易引擎。
// 发送缓冲满时保留队首，完整入缓冲后才 pop 和推进序号；短写由 TCPSocket 继续处理。
// 回报必须匹配版本、身份、epoch 和连续序号；异常连接标记为不确定，不能自动重发有副作用的请求。
// pending_requests_ 等待请求确认，FILLED 不抵扣该计数；ACCEPTED 也不代表订单已经全部成交。

#include "order_gateway.h"

namespace Trading {
OrderGateway::OrderGateway(ClientId client_id,  // 构造客户专用订单网关。
                           // 借用交易引擎生产的请求队列。
                           Exchange::ClientRequestLFQueue* client_requests,
                           // 借用回报队列，网关向其中生产。
                           Exchange::ClientResponseLFQueue* client_responses,
                           // 接收服务器地址、接口配置与端口。
                           std::string ip, const std::string& iface, int port)
    // 保存连接配置和队列指针。
    : client_id_(client_id),
      ip_(ip),
      iface_(iface),
      port_(port),
      outgoing_requests_(client_requests),
      incoming_responses_(client_responses),
      // 创建客户独立日志，再构造使用该日志的 TCP 对象。
      logger_("trading_order_gateway_" + std::to_string(client_id) + ".log"),
      tcp_socket_(logger_) {
  // 将收包回调绑定到此网关。
  tcp_socket_.recv_callback_ = [this](auto socket, auto rx_time) { recvCallback(socket, rx_time); };
}

// 执行订单收发和停机排空。
auto OrderGateway::run() noexcept -> void {
  logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr());

  // 等待请求、发送缓冲、确认以及最后收包后的 50ms 静默窗。
  while (run_ || outgoing_requests_->peek() || tcp_socket_.pending_bytes() || pending_requests_ ||
         Common::getMonotonicNanos() - Nanos(last_receive_) < 50 * NANOS_TO_MILLIS) {
    // 排空超时标记会话不确定，不把未确认请求算成成功。
    if (!run_ && Common::getMonotonicNanos() > stop_deadline_.load()) {
      session_healthy_ = false;
      logger_.log("ORDER SESSION UNKNOWN: shutdown deadline pending:% unsent_bytes:%\n",
                  pending_requests_, tcp_socket_.pending_bytes());
      break;
    }
    tcp_socket_.sendAndRecv();  // 执行网络 I/O，并重试缓冲内旧回报。

    // 检查传输层和协议层健康状态。
    if (!tcp_socket_.healthy() || !session_healthy_.load(std::memory_order_acquire)) {
      logger_.log(
          "ORDER SESSION UNKNOWN: disconnect; stop new sends and reconcile before restart\n");

      // 发布会话失效并停止循环。
      session_healthy_.store(false, std::memory_order_release);
      run_ = false;
      break;
    }

    // 只有一个消费者，peek 非空即可继续。
    for (auto client_request = outgoing_requests_->peek(); client_request;
         client_request = outgoing_requests_->peek()) {
      TTT_MEASURE(T11_OrderGateway_LFQueue_read, logger_);
      logger_.log("%:% %() % Sending cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__,
                  // 补充请求时间、客户和出向序号。
                  Common::getCurrentTimeStr(), client_id_, next_outgoing_seq_num_,
                  client_request->toString());
      START_MEASURE(Trading_TCPSocket_send);

      // 把请求、连续序号和 epoch 编码成固定帧。
      const auto frame = Common::Wire::encode(
          Exchange::OMClientRequest{next_outgoing_seq_num_, *client_request, session_epoch_});

      // 尝试完整进入 TCP 发送缓冲。
      const auto status = tcp_socket_.send(frame.data(), frame.size());
      if (status == Common::SendResult::Full)
        break;  // 满时不 pop，保留原请求重试。

      // 其他失败使订单交付状态不确定。
      ASSERT(status == Common::SendResult::Accepted,
             "order session unknown: cannot queue complete frame");
      END_MEASURE(Trading_TCPSocket_send, logger_);
      outgoing_requests_->pop();  // 入缓冲成功后释放请求队首槽位。
      TTT_MEASURE(T12_OrderGateway_TCP_write, logger_);
      next_outgoing_seq_num_++;
      ++pending_requests_;  // 推进出向序号，并增加等待确认的请求数。
    }
  }

  logger_.log(
      "GATEWAY STATS sent:% received_frames:% pending:% unsent_bytes:% healthy:% response_queue_full:%\n",
      next_outgoing_seq_num_ - 1, next_exp_seq_num_ - 1, pending_requests_,
      tcp_socket_.pending_bytes(), session_healthy_.load(), incoming_responses_->full_count());
}

// 从 TCP 字节流解析私有回报。
auto OrderGateway::recvCallback(TCPSocket* socket, Nanos rx_time) noexcept -> void {
  TTT_MEASURE(T7t_OrderGateway_TCP_read, logger_);
  START_MEASURE(Trading_OrderGateway_recvCallback);
  logger_.log("%:% %() % Received socket:% len:% %\n", __FILE__, __LINE__, __FUNCTION__,
              Common::getCurrentTimeStr(), socket->socket_fd_, socket->next_rcv_valid_index_,
              rx_time);
  size_t consumed = 0;  // 本轮已处理的完整帧总字节数。

  // 不足一整帧则留待后续收包。
  while (consumed + Common::Wire::ResponseSize <= socket->next_rcv_valid_index_) {
    Exchange::OMClientResponse response;  // 保存解码出的回报。

    // 检查编码、会话 epoch 和客户身份。
    if (!Common::Wire::decode(
            reinterpret_cast<const uint8_t*>(socket->inbound_data_.data() + consumed), response) ||
        response.session_epoch_ != session_epoch_ ||
        response.me_client_response_.client_id_ != client_id_) {
      // 不匹配时停用会话和连接，不继续猜测帧内容。
      session_healthy_.store(false, std::memory_order_release);
      socket->state_ = Common::ConnectionState::Error;
      break;
    }

    // 旧序号视为重复帧，跳过但不再次投递。
    if (response.seq_num_ < next_exp_seq_num_) {
      consumed += Common::Wire::ResponseSize;
      continue;
    }

    // 新序号有缺口则进入不确定状态。
    if (response.seq_num_ != next_exp_seq_num_) {
      session_healthy_.store(false, std::memory_order_release);
      socket->state_ = Common::ConnectionState::Error;
      break;
    }
    const auto& r = response.me_client_response_;  // 引用私有业务回报。

    // 回报类型必须位于合法范围。
    if (r.type_ < Exchange::ClientResponseType::ACCEPTED ||
        r.type_ > Exchange::ClientResponseType::STATE ||
        // FILLED 额外检查品种、方向、成交量、响应 ID 和价格。
        (r.type_ == Exchange::ClientResponseType::FILLED &&
         (r.ticker_id_ >= ME_MAX_TICKERS || (r.side_ != Side::BUY && r.side_ != Side::SELL) ||
          !r.exec_qty_ || !r.response_id_ || r.exec_qty_ > INT32_MAX || r.price_ <= 0 ||
          r.price_ == Price_INVALID))) {
      // 非法回报使连接失效。
      session_healthy_.store(false, std::memory_order_release);
      socket->state_ = Common::ConnectionState::Error;
      break;
    }

    if (!incoming_responses_->try_push(r))
      break;  // 下游队列满时保留当前帧与序号，下轮重试。

    // 会话类拒绝会关闭订单发送门禁。
    if (r.reject_reason_ == Exchange::RejectReason::IDENTITY ||
        r.reject_reason_ == Exchange::RejectReason::SEQUENCE ||
        r.reject_reason_ == Exchange::RejectReason::SESSION ||
        r.reject_reason_ == Exchange::RejectReason::VERSION)
      session_healthy_.store(false, std::memory_order_release);
    last_receive_ = Common::getMonotonicNanos();  // 记录最近接收时刻，使用单调时钟。

    // 非 FILLED 回报抵扣请求确认数，成交事件不能重复抵扣。
    if (r.type_ != Exchange::ClientResponseType::FILLED && pending_requests_)
      --pending_requests_;

    // 成功投递后推进回报序号和消费字节数。
    ++next_exp_seq_num_;
    consumed += Common::Wire::ResponseSize;
  }

  // 将未消费的完整帧或半帧移到缓冲开头。
  if (consumed) {
    memmove(socket->inbound_data_.data(), socket->inbound_data_.data() + consumed,
            socket->next_rcv_valid_index_ - consumed);
    socket->next_rcv_valid_index_ -= consumed;
  }
  END_MEASURE(Trading_OrderGateway_recvCallback, logger_);
}
}
