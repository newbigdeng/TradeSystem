// 订单入口：切帧 → 校验版本、身份、会话和序号 → 定序暂存 → 撮合队列。
// 下游满时不消耗当前帧；只有请求已暂存或明确拒绝成功，才移动 TCP 缓冲边界。
// quiesce 的主线程只等原子 quiesced_；pending() 在服务器工作线程内读取，没有这里曾注释的跨线程裸读。
// 停机顺序为 quiesce → 停撮合 → stop，保证已接受请求与待发回报得到排空机会。
// 断线后的 QUERY 接管仍保持会话不确定状态，不代表已实现自动安全重连。

#pragma once // 防止头文件重复包含。
#include "common/critical_journal.h" // 引入common/critical_journal.h 的声明。
#include <unordered_map> // 引入哈希映射。
#include "common/order_protocol.h" // 引入common/order_protocol.h 的声明。
#include <functional> // 引入回调函数封装。
#include "common/thread_utils.h" // 引入common/thread_utils.h 的声明。
#include "common/macros.h" // 引入common/macros.h 的声明。
#include "common/tcp_server.h" // 引入common/tcp_server.h 的声明。
#include "order_server/client_request.h" // 引入order_server/client_request.h 的声明。
#include "order_server/client_response.h" // 引入order_server/client_response.h 的声明。
#include "order_server/fifo_sequencer.h" // 引入order_server/fifo_sequencer.h 的声明。
namespace Exchange { // 所属命名空间。
  class OrderServer { // 定义OrderServer 的接口和状态。
  public: // 对外接口。
    OrderServer(ClientRequestLFQueue *client_requests, ClientResponseLFQueue *client_responses, const std::string &iface,int port,Common::CriticalJournal* audit=nullptr); // 构造时借用请求/回报队列，可选持久审计。
    ~OrderServer(); // 析构会停止并等待工作线程。
    auto start() -> void; // 启动监听和工作线程。
    auto stop() -> void; // 停止接收并等待输出排空。
    void quiesce() { // 停机第一阶段，等待已暂存请求进入撮合队列。
      accepting_.store(false,std::memory_order_release); // 发布停止接收新业务的标志。
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5); // 用单调时钟设置五秒排空期限。
      while(!quiesced_.load(std::memory_order_acquire)) { // 主线程只等待原子确认，不读取 pending_size_。
        ASSERT(std::chrono::steady_clock::now()<deadline,"cannot drain accepted FIFO requests on shutdown");std::this_thread::yield(); // 超时明确失败；等待期间让出调度机会。
      } // 结束当前作用域。
    } // 结束当前作用域。

    auto run() noexcept { // 订单服务器工作线程主循环。
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr()); // 记录诊断日志。
      while(run_ || outgoing_responses_->peek() || tcp_server_.pendingBytes()) { // 继续运行或仍有队列/网络缓冲待排空。
        if(!run_ && Common::getMonotonicNanos()>stop_deadline_.load())FATAL("shutdown has unacknowledged exchange output; reconciliation required"); // 不能在未确认输出状态下假装正常停机。
        if(accepting_.load(std::memory_order_acquire))tcp_server_.poll(); // 允许接收时查询新连接和就绪事件。
        tcp_server_.sendAndRecv(); // 持续处理连接 I/O 和暂存帧重试。
        if(!accepting_.load(std::memory_order_acquire) && !fifo_sequencer_.pending())quiesced_.store(true,std::memory_order_release); // 所属线程确认暂存请求排空，再用 release 通知主线程。
        bool queued_response = false; // 标记本轮是否新增待发回报。
        for (auto client_response = outgoing_responses_->peek(); client_response; client_response = outgoing_responses_->peek()) { // peek 非空即有回报，无需再查询队列 size。
          TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_); // 可选性能计时点，是否启用由构建配置决定。
          ASSERT(client_response->client_id_<ME_MAX_NUM_CLIENTS,"response has invalid client ID"); // 在用客户 ID 下标前检查范围。
          auto &next_outgoing_seq_num = cid_next_outgoing_seq_num_[client_response->client_id_]; // 取得该客户下一条出向序号。
          logger_.log("%:% %() % Processing cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(), // 记录诊断日志。
                      client_response->client_id_, next_outgoing_seq_num, client_response->toString()); // 补充回报的客户、序号和内容。
          ASSERT(cid_tcp_socket_[client_response->client_id_] != nullptr, // 回报必须存在对应的客户连接。
                 "Dont have a TCPSocket for ClientId:" + std::to_string(client_response->client_id_)); // 缺少连接时记录客户 ID。
          START_MEASURE(Exchange_TCPSocket_send); // 可选性能计时点，是否启用由构建配置决定。
          const auto frame=Common::Wire::encode(OMClientResponse{next_outgoing_seq_num,*client_response,cid_session_epoch_[client_response->client_id_]}); // 编码版本化回报帧，并带上所属会话 epoch。
          const auto status=cid_tcp_socket_[client_response->client_id_]->send(frame.data(),frame.size()); // 尝试完整加入该连接的发送缓冲。
          if(status==Common::SendResult::Full) break; // 缓冲满则保留队首，下轮重试。
          ASSERT(status==Common::SendResult::Accepted,"response delivery unknown; matching must stop for reconciliation"); // 非满却发送失败，交付状态不确定，必须停止对账。
          END_MEASURE(Exchange_TCPSocket_send, logger_); // 可选性能计时点，是否启用由构建配置决定。
          queued_response = true; // 该完整回报已经进入 TCP 用户态发送缓冲。
          outgoing_responses_->pop(); // 完整入发送缓冲后才归还上游槽位。
          TTT_MEASURE(T6t_OrderServer_TCP_write, logger_); // 可选性能计时点，是否启用由构建配置决定。
          ++next_outgoing_seq_num; // 只对已入缓冲的回报推进出向序号。
        } // 结束当前作用域。
        if (queued_response) tcp_server_.flushSend(); // 本轮尝试发送，不让新请求审计先占用下一轮。
      } // 结束当前作用域。
    } // 结束当前作用域。

    bool reject(TCPSocket *socket,const OMClientRequest& request,RejectReason reason) noexcept { // 发送明确拒绝；缓冲满时返回 false 让调用者保留输入帧。
      const auto &r=request.me_client_request_; // 引用请求业务字段。
      const auto owner=socket_client_.find(socket); // 查询当前连接已绑定的客户身份。
      auto &sequence=owner!=socket_client_.end()?cid_next_outgoing_seq_num_[owner->second]:anonymous_sequences_.try_emplace(socket,1).first->second; // 已绑定连接使用客户序号，否则使用匿名连接序号。
      MEClientResponse response{ClientResponseType::REJECTED,r.client_id_,r.ticker_id_,r.order_id_,OrderId_INVALID,r.side_,r.price_,0,0,reason}; // 构造带拒绝原因的私有回报。
      if(owner!=socket_client_.end())response.client_id_=owner->second; // 防止伪造请求中的客户 ID 改变回报身份。
      const auto bytes=Common::Wire::encode(OMClientResponse{sequence,response,request.session_epoch_}); // 把拒绝回报编码成协议帧。
      const auto result=socket->send(bytes.data(),bytes.size()); // 尝试完整加入网络发送缓冲。
      if(result==Common::SendResult::Full)return false; // 缓冲满，不推进输入或输出序号。
      ASSERT(result==Common::SendResult::Accepted,"cannot send explicit protocol rejection"); // 其他发送失败不能假装拒绝已经送出。
      if(audit_)audit_->response(response); // 按配置记录持久审计。
      ++sequence;++rejected_frames_;return true; // 拒绝成功入缓冲后更新计数和序号。
    } // 结束当前作用域。

    auto recvCallback(TCPSocket *socket,Nanos rx_time) noexcept { // 解析 TCP 累积缓冲中的完整请求帧。
      if(!accepting_.load(std::memory_order_acquire))return; // 静默阶段不再接受新业务请求。
      size_t consumed=0; // 记录本轮可从缓冲移除的字节数。
      while(consumed+Common::Wire::RequestSize<=socket->next_rcv_valid_index_) { // 只解析完整 RequestSize 帧，半帧留待补齐。
        OMClientRequest request; // 接收解码结果。
        auto reason=RejectReason::NONE; // 初始没有协议拒绝原因。
        if(!Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+consumed),request))reason=RejectReason::VERSION; // 显式解码失败标记为版本/格式错误。
        const auto &r=request.me_client_request_; // 引用解码后的业务字段。
        if(reason==RejectReason::NONE) { // 格式正确才继续身份与会话检查。
          if(r.client_id_>=ME_MAX_NUM_CLIENTS)reason=RejectReason::INVALID_ID; // 客户 ID 必须在映射范围内。
          else { // 进入另一分支。
            const auto bound=socket_client_.find(socket); // 检查本 TCP 连接是否已经绑定身份。
            if(bound!=socket_client_.end() && bound->second!=r.client_id_)reason=RejectReason::IDENTITY; // 同一连接不能更换客户 ID。
            auto *owner=cid_tcp_socket_[r.client_id_]; // 查找该客户当前绑定的连接。
            if(reason==RejectReason::NONE && owner && owner!=socket) { // 已有其他连接占用该客户身份。
              if(!owner->healthy() && r.type_==ClientRequestType::QUERY && request.session_epoch_==cid_session_epoch_[r.client_id_]) { // 只有旧连接失效、QUERY 和相同 epoch 才允许查询接管。
                cid_tcp_socket_[r.client_id_]=socket;socket_client_[socket]=r.client_id_;cid_session_unknown_[r.client_id_]=true; // 绑定查询连接，并保持会话不确定状态。
                cid_next_exp_seq_num_[r.client_id_]=request.seq_num_;cid_next_outgoing_seq_num_[r.client_id_]=1; // 采用查询请求序号，重置该连接的回报序号；不等于实现自动重连。
              } else reason=RejectReason::IDENTITY; // 其他身份争用明确拒绝。
            } // 结束当前作用域。

            if(reason==RejectReason::NONE && cid_tcp_socket_[r.client_id_] && request.session_epoch_!=cid_session_epoch_[r.client_id_])reason=RejectReason::SESSION; // 已绑定会话必须匹配 epoch。
            if(reason==RejectReason::NONE && !request.session_epoch_)reason=RejectReason::SESSION; // 拒绝零 epoch。
            if(reason==RejectReason::NONE && request.seq_num_!=cid_next_exp_seq_num_[r.client_id_])reason=RejectReason::SEQUENCE; // 请求序号必须等于期望值。
            if(reason==RejectReason::NONE && cid_session_unknown_[r.client_id_] && r.type_!=ClientRequestType::QUERY)reason=RejectReason::SESSION; // 不确定会话仅允许 QUERY，禁止新单和撤单。
          } // 结束当前作用域。
        } // 结束当前作用域。

        const auto invalid=Common::Wire::validate(r); // 检查请求类型、价格、数量等业务字段。
        if(reason==RejectReason::NONE && invalid!=RejectReason::NONE) { // 协议有效但业务字段无效。
          if(!cid_tcp_socket_[r.client_id_]) { // 首次有效会话请求需要建立身份映射。
            cid_tcp_socket_[r.client_id_]=socket;cid_session_epoch_[r.client_id_]=request.session_epoch_;socket_client_[socket]=r.client_id_; // 登记客户、socket 和 epoch。
            cid_next_outgoing_seq_num_[r.client_id_]=anonymous_sequences_.try_emplace(socket,1).first->second; // 沿用此前匿名拒绝消耗过的出向序号。
          } // 结束当前作用域。

          if(!reject(socket,request,invalid))break; // 拒绝缓冲满时保留该帧，不继续推进。
          ++cid_next_exp_seq_num_[r.client_id_]; // 业务字段拒绝也消耗合法请求序号。
        } else if(reason!=RejectReason::NONE) { // 处理身份、会话或协议层错误。
          if(!reject(socket,request,reason))break; // 拒绝输出受背压时保留当前输入。
        } else { // 进入另一分支。
          if(!fifo_sequencer_.canAccept())break; // 定序器满时保留该帧，暂停解析。
          if(audit_)audit_->request("RECEIVED",r); // 先记录收到的请求审计。
          ASSERT(fifo_sequencer_.addClientRequest(rx_time,r),"FIFO capacity changed within single owner"); // 同线程预检通过后暂存；容量意外变化是程序错误。
          if(!cid_tcp_socket_[r.client_id_]) { // 首次接受请求时建立连接身份。
            cid_tcp_socket_[r.client_id_]=socket;cid_session_epoch_[r.client_id_]=request.session_epoch_;socket_client_[socket]=r.client_id_; // 保存客户连接、epoch 和反向映射。
            cid_next_outgoing_seq_num_[r.client_id_]=anonymous_sequences_.try_emplace(socket,1).first->second; // 继续使用该连接已有的出向序号。
          } // 结束当前作用域。

          ++cid_next_exp_seq_num_[r.client_id_];++accepted_frames_; // 请求成功暂存后推进期望序号和接受计数。
        } // 结束当前作用域。

        consumed+=Common::Wire::RequestSize; // 当前帧处理完成，累计可移除字节数。
      } // 结束当前作用域。

      if(consumed) {memmove(socket->inbound_data_.data(),socket->inbound_data_.data()+consumed,socket->next_rcv_valid_index_-consumed);socket->next_rcv_valid_index_-=consumed;} // 压紧未消费尾部；既保留半帧，也保留因背压暂停的完整帧。
    } // 结束当前作用域。

    auto recvFinishedCallback() noexcept { // 一轮收包结束后触发定序和重试。
      START_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish); // 可选性能计时点，是否启用由构建配置决定。
      fifo_sequencer_.sequenceAndPublish(); // 排序并尽可能发布暂存请求。
      END_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish, logger_); // 可选性能计时点，是否启用由构建配置决定。
    } // 结束当前作用域。

    OrderServer() = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderServer(const OrderServer &) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderServer(const OrderServer &&) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderServer &operator=(const OrderServer &) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderServer &operator=(const OrderServer &&) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
  private: // 内部状态与辅助接口。
    const std::string iface_; // 监听接口名称。
    const int port_ = 0; // 监听端口。
    ClientResponseLFQueue *outgoing_responses_ = nullptr; // 撮合引擎生产、网络线程消费的回报队列。
    std::atomic<bool> run_{false}; // 跨线程运行标志。
    std::unique_ptr<std::thread> worker_; // 管理网络工作线程的生命周期。
    std::string time_str_; // 日志时间格式化缓冲。
    Logger logger_; // 日志对象；声明在网络组件前以先构造。
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_outgoing_seq_num_; // 按客户记录下一条出向回报序号。
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_exp_seq_num_; // 按客户记录下一条期望请求序号。
    std::array<Common::TCPSocket *, ME_MAX_NUM_CLIENTS> cid_tcp_socket_; // 客户到 TCP 连接的映射，构造函数填 nullptr。
    std::array<uint64_t,ME_MAX_NUM_CLIENTS> cid_session_epoch_{}; // 各客户会话 epoch，初始为零。
    std::array<bool,ME_MAX_NUM_CLIENTS> cid_session_unknown_{}; // 会话不确定标记；当前查询接管不会清除它。
    std::unordered_map<TCPSocket*,uint64_t> anonymous_sequences_; // 尚未绑定客户的连接也需要连续回报序号。
    std::unordered_map<TCPSocket*,ClientId> socket_client_; // 连接到客户的反向映射，限制冒用身份。
    uint64_t accepted_frames_=0,rejected_frames_=0; // 工作线程维护接受和拒绝帧计数。
    Common::TCPServer tcp_server_; // TCP 监听和连接管理器。
    FIFOSequencer fifo_sequencer_; // 工作线程内的跨连接定序器。
    Common::CriticalJournal* audit_=nullptr; // 可选审计对象，生命周期由上层管理。
    std::atomic<bool> accepting_{true},quiesced_{false}; // 接收开关与静默完成确认，均为原子变量。
    std::atomic<Nanos> stop_deadline_{INT64_MAX}; // 停止后的输出排空期限。
  }; // 结束当前作用域。
} // 结束当前作用域。
