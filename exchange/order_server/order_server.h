#pragma once
#include "common/critical_journal.h"
#include <unordered_map>
#include "common/order_protocol.h"

#include <functional>

#include "common/thread_utils.h"
#include "common/macros.h"
#include "common/tcp_server.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "order_server/fifo_sequencer.h"

namespace Exchange {
  class OrderServer {
  public:
    OrderServer(ClientRequestLFQueue *client_requests, ClientResponseLFQueue *client_responses, const std::string &iface,int port,Common::CriticalJournal* audit=nullptr);

    ~OrderServer();

    /// Start and stop the order server main thread.
    auto start() -> void;

    auto stop() -> void;

    void quiesce() {
      accepting_.store(false,std::memory_order_release);
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
      while(!quiesced_.load(std::memory_order_acquire)) {
        ASSERT(std::chrono::steady_clock::now()<deadline,"cannot drain accepted FIFO requests on shutdown");std::this_thread::yield();
      }
    }
    /// Main run loop for this thread - accepts new client connections, receives client requests from them and sends client responses to them.
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      while(run_ || outgoing_responses_->peek() || tcp_server_.pendingBytes()) {
        if(!run_ && Common::getMonotonicNanos()>stop_deadline_.load())FATAL("shutdown has unacknowledged exchange output; reconciliation required");
        if(accepting_.load(std::memory_order_acquire))tcp_server_.poll();

        tcp_server_.sendAndRecv();
        if(!accepting_.load(std::memory_order_acquire) && !fifo_sequencer_.pending())quiesced_.store(true,std::memory_order_release);

        for (auto client_response = outgoing_responses_->peek(); outgoing_responses_->size() && client_response; client_response = outgoing_responses_->peek()) {
          TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_);

          ASSERT(client_response->client_id_<ME_MAX_NUM_CLIENTS,"response has invalid client ID");
          auto &next_outgoing_seq_num = cid_next_outgoing_seq_num_[client_response->client_id_];
          logger_.log("%:% %() % Processing cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                      client_response->client_id_, next_outgoing_seq_num, client_response->toString());

          ASSERT(cid_tcp_socket_[client_response->client_id_] != nullptr,
                 "Dont have a TCPSocket for ClientId:" + std::to_string(client_response->client_id_));
          START_MEASURE(Exchange_TCPSocket_send);
          const auto frame=Common::Wire::encode(OMClientResponse{next_outgoing_seq_num,*client_response,cid_session_epoch_[client_response->client_id_]});
          const auto status=cid_tcp_socket_[client_response->client_id_]->send(frame.data(),frame.size());
          if(status==Common::SendResult::Full) break;
          ASSERT(status==Common::SendResult::Accepted,"response delivery unknown; matching must stop for reconciliation");
          END_MEASURE(Exchange_TCPSocket_send, logger_);

          outgoing_responses_->pop();
          TTT_MEASURE(T6t_OrderServer_TCP_write, logger_);

          ++next_outgoing_seq_num;
        }
      }
    }

    /// Read client request from the TCP receive buffer, check for sequence gaps and forward it to the FIFO sequencer.
    bool reject(TCPSocket *socket,const OMClientRequest& request,RejectReason reason) noexcept {
      const auto &r=request.me_client_request_;
      const auto owner=socket_client_.find(socket);
      auto &sequence=owner!=socket_client_.end()?cid_next_outgoing_seq_num_[owner->second]:anonymous_sequences_.try_emplace(socket,1).first->second;
      MEClientResponse response{ClientResponseType::REJECTED,r.client_id_,r.ticker_id_,r.order_id_,OrderId_INVALID,r.side_,r.price_,0,0,reason};
      if(owner!=socket_client_.end())response.client_id_=owner->second;
      const auto bytes=Common::Wire::encode(OMClientResponse{sequence,response,request.session_epoch_});
      const auto result=socket->send(bytes.data(),bytes.size());
      if(result==Common::SendResult::Full)return false;
      ASSERT(result==Common::SendResult::Accepted,"cannot send explicit protocol rejection");
      if(audit_)audit_->response(response);
      ++sequence;++rejected_frames_;return true;
    }
    auto recvCallback(TCPSocket *socket,Nanos rx_time) noexcept {
      if(!accepting_.load(std::memory_order_acquire))return;
      size_t consumed=0;
      while(consumed+Common::Wire::RequestSize<=socket->next_rcv_valid_index_) {
        OMClientRequest request;
        auto reason=RejectReason::NONE;
        if(!Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+consumed),request))reason=RejectReason::VERSION;
        const auto &r=request.me_client_request_;
        if(reason==RejectReason::NONE) {
          if(r.client_id_>=ME_MAX_NUM_CLIENTS)reason=RejectReason::INVALID_ID;
          else {
            const auto bound=socket_client_.find(socket);
            if(bound!=socket_client_.end() && bound->second!=r.client_id_)reason=RejectReason::IDENTITY;
            auto *owner=cid_tcp_socket_[r.client_id_];
            if(reason==RejectReason::NONE && owner && owner!=socket) {
              // A disconnected account can reconnect only for read-only queries
              // using its previous epoch. New economic intent stays blocked.
              if(!owner->healthy() && r.type_==ClientRequestType::QUERY && request.session_epoch_==cid_session_epoch_[r.client_id_]) {
                cid_tcp_socket_[r.client_id_]=socket;socket_client_[socket]=r.client_id_;cid_session_unknown_[r.client_id_]=true;
                cid_next_exp_seq_num_[r.client_id_]=request.seq_num_;cid_next_outgoing_seq_num_[r.client_id_]=1;
              } else reason=RejectReason::IDENTITY;
            }
            if(reason==RejectReason::NONE && cid_tcp_socket_[r.client_id_] && request.session_epoch_!=cid_session_epoch_[r.client_id_])reason=RejectReason::SESSION;
            if(reason==RejectReason::NONE && !request.session_epoch_)reason=RejectReason::SESSION;
            if(reason==RejectReason::NONE && request.seq_num_!=cid_next_exp_seq_num_[r.client_id_])reason=RejectReason::SEQUENCE;
            if(reason==RejectReason::NONE && cid_session_unknown_[r.client_id_] && r.type_!=ClientRequestType::QUERY)reason=RejectReason::SESSION;
          }
        }
        const auto invalid=Common::Wire::validate(r);
        if(reason==RejectReason::NONE && invalid!=RejectReason::NONE) {
          if(!cid_tcp_socket_[r.client_id_]) {
            cid_tcp_socket_[r.client_id_]=socket;cid_session_epoch_[r.client_id_]=request.session_epoch_;socket_client_[socket]=r.client_id_;
            cid_next_outgoing_seq_num_[r.client_id_]=anonymous_sequences_.try_emplace(socket,1).first->second;
          }
          if(!reject(socket,request,invalid))break;
          ++cid_next_exp_seq_num_[r.client_id_];
        } else if(reason!=RejectReason::NONE) {
          if(!reject(socket,request,reason))break;
        } else {
          if(!fifo_sequencer_.canAccept())break;
          if(audit_)audit_->request("RECEIVED",r);
          ASSERT(fifo_sequencer_.addClientRequest(rx_time,r),"FIFO capacity changed within single owner");
          if(!cid_tcp_socket_[r.client_id_]) {
            cid_tcp_socket_[r.client_id_]=socket;cid_session_epoch_[r.client_id_]=request.session_epoch_;socket_client_[socket]=r.client_id_;
            cid_next_outgoing_seq_num_[r.client_id_]=anonymous_sequences_.try_emplace(socket,1).first->second;
          }
          ++cid_next_exp_seq_num_[r.client_id_];++accepted_frames_;
        }
        consumed+=Common::Wire::RequestSize;
      }
      if(consumed) {memmove(socket->inbound_data_.data(),socket->inbound_data_.data()+consumed,socket->next_rcv_valid_index_-consumed);socket->next_rcv_valid_index_-=consumed;}
    }

    /// End of reading incoming messages across all the TCP connections, sequence and publish the client requests to the matching engine.
    auto recvFinishedCallback() noexcept {
      START_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish);
      fifo_sequencer_.sequenceAndPublish();
      END_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish, logger_);
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    OrderServer() = delete;

    OrderServer(const OrderServer &) = delete;

    OrderServer(const OrderServer &&) = delete;

    OrderServer &operator=(const OrderServer &) = delete;

    OrderServer &operator=(const OrderServer &&) = delete;

  private:
    const std::string iface_;
    const int port_ = 0;

    /// Lock free queue of outgoing client responses to be sent out to connected clients.
    ClientResponseLFQueue *outgoing_responses_ = nullptr;

    std::atomic<bool> run_{false};
    std::unique_ptr<std::thread> worker_;

    std::string time_str_;
    Logger logger_;

    /// Hash map from ClientId -> the next sequence number to be sent on outgoing client responses.
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_outgoing_seq_num_;

    /// Hash map from ClientId -> the next sequence number expected on incoming client requests.
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_exp_seq_num_;

    /// Hash map from ClientId -> TCP socket / client connection.
    std::array<Common::TCPSocket *, ME_MAX_NUM_CLIENTS> cid_tcp_socket_;
    std::array<uint64_t,ME_MAX_NUM_CLIENTS> cid_session_epoch_{};
    std::array<bool,ME_MAX_NUM_CLIENTS> cid_session_unknown_{};
    std::unordered_map<TCPSocket*,uint64_t> anonymous_sequences_;
    std::unordered_map<TCPSocket*,ClientId> socket_client_;
    uint64_t accepted_frames_=0,rejected_frames_=0;

    /// TCP server instance listening for new client connections.
    Common::TCPServer tcp_server_;

    /// FIFO sequencer responsible for making sure incoming client requests are processed in the order in which they were received.
    FIFOSequencer fifo_sequencer_;
    Common::CriticalJournal* audit_=nullptr;
    std::atomic<bool> accepting_{true},quiesced_{false};
    std::atomic<Nanos> stop_deadline_{INT64_MAX};
  };
}
