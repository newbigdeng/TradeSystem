// 订单网关把请求编码为 TCP 帧，并把私有回报交给交易引擎。
// 发送缓冲满时保留队首，完整入缓冲后才 pop 和推进序号；短写由 TCPSocket 继续处理。
// 回报必须匹配版本、身份、epoch 和连续序号；异常连接标记为不确定，不能自动重发有副作用的请求。
// pending_requests_ 等待请求确认，FILLED 不抵扣该计数；ACCEPTED 也不代表订单已经全部成交。

#include "order_gateway.h" // 引入order_gateway.h 的声明。
namespace Trading { // 所属命名空间。
  OrderGateway::OrderGateway(ClientId client_id, // 构造客户专用订单网关。
                             Exchange::ClientRequestLFQueue *client_requests, // 借用交易引擎生产的请求队列。
                             Exchange::ClientResponseLFQueue *client_responses, // 借用回报队列，网关向其中生产。
                             std::string ip, const std::string &iface, int port) // 接收服务器地址、接口配置与端口。
      : client_id_(client_id), ip_(ip), iface_(iface), port_(port), outgoing_requests_(client_requests), incoming_responses_(client_responses), // 保存连接配置和队列指针。
      logger_("trading_order_gateway_" + std::to_string(client_id) + ".log"), tcp_socket_(logger_) { // 创建客户独立日志，再构造使用该日志的 TCP 对象。
    tcp_socket_.recv_callback_ = [this](auto socket, auto rx_time) { recvCallback(socket, rx_time); }; // 将收包回调绑定到此网关。
  } // 结束当前作用域。

  auto OrderGateway::run() noexcept -> void { // 执行订单收发和停机排空。
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr()); // 记录诊断日志。
    while(run_ || outgoing_requests_->peek() || tcp_socket_.pending_bytes() || pending_requests_ || Common::getMonotonicNanos()-Nanos(last_receive_)<50*NANOS_TO_MILLIS) { // 等待请求、发送缓冲、确认以及最后收包后的 50ms 静默窗。
      if(!run_ && Common::getMonotonicNanos()>stop_deadline_.load()) {session_healthy_=false;logger_.log("ORDER SESSION UNKNOWN: shutdown deadline pending:% unsent_bytes:%\n",pending_requests_,tcp_socket_.pending_bytes());break;} // 排空超时标记会话不确定，不把未确认请求算成成功。
      tcp_socket_.sendAndRecv(); // 执行网络 I/O，并重试缓冲内旧回报。
      if(!tcp_socket_.healthy() || !session_healthy_.load(std::memory_order_acquire)) { // 检查传输层和协议层健康状态。
        logger_.log("ORDER SESSION UNKNOWN: disconnect; stop new sends and reconcile before restart\n"); // 记录诊断日志。
        session_healthy_.store(false,std::memory_order_release);run_=false; break; // 发布会话失效并停止循环。
      } // 结束当前作用域。

      for(auto client_request = outgoing_requests_->peek(); client_request; client_request = outgoing_requests_->peek()) { // 只有一个消费者，peek 非空即可继续。
        TTT_MEASURE(T11_OrderGateway_LFQueue_read, logger_); // 可选性能计时点，是否启用由构建配置决定。
        logger_.log("%:% %() % Sending cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__, // 记录诊断日志。
                    Common::getCurrentTimeStr(), client_id_, next_outgoing_seq_num_, client_request->toString()); // 补充请求时间、客户和出向序号。
        START_MEASURE(Trading_TCPSocket_send); // 可选性能计时点，是否启用由构建配置决定。
        const auto frame=Common::Wire::encode(Exchange::OMClientRequest{next_outgoing_seq_num_,*client_request,session_epoch_}); // 把请求、连续序号和 epoch 编码成固定帧。
        const auto status=tcp_socket_.send(frame.data(),frame.size()); // 尝试完整进入 TCP 发送缓冲。
        if(status==Common::SendResult::Full) break; // 满时不 pop，保留原请求重试。
        ASSERT(status==Common::SendResult::Accepted,"order session unknown: cannot queue complete frame"); // 其他失败使订单交付状态不确定。
        END_MEASURE(Trading_TCPSocket_send, logger_); // 可选性能计时点，是否启用由构建配置决定。
        outgoing_requests_->pop(); // 入缓冲成功后释放请求队首槽位。
        TTT_MEASURE(T12_OrderGateway_TCP_write, logger_); // 可选性能计时点，是否启用由构建配置决定。
        next_outgoing_seq_num_++;++pending_requests_; // 推进出向序号，并增加等待确认的请求数。
      } // 结束当前作用域。
    } // 结束当前作用域。

    logger_.log("GATEWAY STATS sent:% received_frames:% pending:% unsent_bytes:% healthy:% response_queue_full:%\n",next_outgoing_seq_num_-1,next_exp_seq_num_-1,pending_requests_,tcp_socket_.pending_bytes(),session_healthy_.load(),incoming_responses_->full_count()); // 记录诊断日志。
  } // 结束当前作用域。

  auto OrderGateway::recvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void { // 从 TCP 字节流解析私有回报。
    TTT_MEASURE(T7t_OrderGateway_TCP_read, logger_); // 可选性能计时点，是否启用由构建配置决定。
    START_MEASURE(Trading_OrderGateway_recvCallback); // 可选性能计时点，是否启用由构建配置决定。
    logger_.log("%:% %() % Received socket:% len:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(), socket->socket_fd_, socket->next_rcv_valid_index_, rx_time); // 记录诊断日志。
    size_t consumed=0; // 本轮已处理的完整帧总字节数。
    while(consumed+Common::Wire::ResponseSize<=socket->next_rcv_valid_index_) { // 不足一整帧则留待后续收包。
      Exchange::OMClientResponse response; // 保存解码出的回报。
      if(!Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+consumed),response) || response.session_epoch_!=session_epoch_ || response.me_client_response_.client_id_!=client_id_) { // 检查编码、会话 epoch 和客户身份。
        session_healthy_.store(false,std::memory_order_release);socket->state_=Common::ConnectionState::Error;break; // 不匹配时停用会话和连接，不继续猜测帧内容。
      } // 结束当前作用域。

      if(response.seq_num_<next_exp_seq_num_) {consumed+=Common::Wire::ResponseSize;continue;} // 旧序号视为重复帧，跳过但不再次投递。
      if(response.seq_num_!=next_exp_seq_num_) {session_healthy_.store(false,std::memory_order_release);socket->state_=Common::ConnectionState::Error;break;} // 新序号有缺口则进入不确定状态。
      const auto &r=response.me_client_response_; // 引用私有业务回报。
      if(r.type_<Exchange::ClientResponseType::ACCEPTED || r.type_>Exchange::ClientResponseType::STATE || // 回报类型必须位于合法范围。
         (r.type_==Exchange::ClientResponseType::FILLED && (r.ticker_id_>=ME_MAX_TICKERS || (r.side_!=Side::BUY && r.side_!=Side::SELL) || !r.exec_qty_ || !r.response_id_ || r.exec_qty_>INT32_MAX || r.price_<=0 || r.price_==Price_INVALID))) { // FILLED 额外检查品种、方向、成交量、响应 ID 和价格。
        session_healthy_.store(false,std::memory_order_release);socket->state_=Common::ConnectionState::Error;break; // 非法回报使连接失效。
      } // 结束当前作用域。

      if(!incoming_responses_->try_push(r))break; // 下游队列满时保留当前帧与序号，下轮重试。
      if(r.reject_reason_==Exchange::RejectReason::IDENTITY || r.reject_reason_==Exchange::RejectReason::SEQUENCE || r.reject_reason_==Exchange::RejectReason::SESSION || r.reject_reason_==Exchange::RejectReason::VERSION)session_healthy_.store(false,std::memory_order_release); // 会话类拒绝会关闭订单发送门禁。
      last_receive_=Common::getMonotonicNanos(); // 记录最近接收时刻，使用单调时钟。
      if(r.type_!=Exchange::ClientResponseType::FILLED && pending_requests_)--pending_requests_; // 非 FILLED 回报抵扣请求确认数，成交事件不能重复抵扣。
      ++next_exp_seq_num_;consumed+=Common::Wire::ResponseSize; // 成功投递后推进回报序号和消费字节数。
    } // 结束当前作用域。

    if(consumed) {memmove(socket->inbound_data_.data(),socket->inbound_data_.data()+consumed,socket->next_rcv_valid_index_-consumed);socket->next_rcv_valid_index_-=consumed;} // 将未消费的完整帧或半帧移到缓冲开头。
    END_MEASURE(Trading_OrderGateway_recvCallback, logger_); // 可选性能计时点，是否启用由构建配置决定。
  } // 结束当前作用域。
} // 结束当前作用域。
