#include "market_data_publisher.h"
#include "common/market_protocol.h"

namespace Exchange {
MarketDataPublisher::MarketDataPublisher(MEMarketUpdateLFQueue* market_updates,
                                         const std::string& iface, const std::string& snapshot_ip,
                                         int snapshot_port, const std::string& incremental_ip,
                                         int incremental_port)
    : outgoing_md_updates_(market_updates),
      snapshot_md_updates_(Common::QueueCapacity<ME_MAX_MARKET_UPDATES>{}),
      run_(false),
      logger_("exchange_market_data_publisher.log"),
      incremental_socket_(logger_) {
  ASSERT(incremental_socket_.init(incremental_ip, iface, incremental_port,
                                  /*是否接收*/ false) >= 0,
         "Unable to create incremental mcast socket. error:" + std::string(std::strerror(errno)));
  snapshot_synthesizer_ =
      new SnapshotSynthesizer(&snapshot_md_updates_, iface, snapshot_ip, snapshot_port);
}

// 从撮合队列取行情，发布增量组播，再交给快照合成器。

auto MarketDataPublisher::run() noexcept -> void {
  logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr());
  while (run_ || outgoing_md_updates_->peek()) {
    for (auto market_update = outgoing_md_updates_->peek(); market_update;
         market_update = outgoing_md_updates_->peek()) {
      TTT_MEASURE(T5_MarketDataPublisher_LFQueue_read, logger_);

      logger_.log("%:% %() % Sending seq:% %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(), next_inc_seq_num_,
                  market_update->toString().c_str());

      START_MEASURE(Exchange_McastSocket_send);
      const auto bytes = Common::Wire::encode(MDPMarketUpdate{next_inc_seq_num_, *market_update});
      if (!incremental_socket_.send(bytes.data(), bytes.size()))
        break;
      END_MEASURE(Exchange_McastSocket_send, logger_);

      const auto update_copy = *market_update;
      outgoing_md_updates_->pop();
      TTT_MEASURE(T6_MarketDataPublisher_UDP_write, logger_);

      // 将增量交给快照合成器。
      const MDPMarketUpdate snapshot_update{next_inc_seq_num_, update_copy};
      ASSERT(snapshot_md_updates_.try_push(snapshot_update),
             "snapshot queue full; matching state cannot be represented");

      ++next_inc_seq_num_;
    }

    // 发布增量组播。
    incremental_socket_.sendAndRecv();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (incremental_socket_.next_send_valid_index_) {
    ASSERT(std::chrono::steady_clock::now() < deadline, "multicast output not drained at shutdown");
    incremental_socket_.sendAndRecv();
    std::this_thread::yield();
  }
}
}
