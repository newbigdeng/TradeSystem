#include "market_data_publisher.h"
#include "common/market_protocol.h"

namespace Exchange {
  MarketDataPublisher::MarketDataPublisher(MEMarketUpdateLFQueue *market_updates, const std::string &iface,
                                           const std::string &snapshot_ip, int snapshot_port,
                                           const std::string &incremental_ip, int incremental_port)
      : outgoing_md_updates_(market_updates), snapshot_md_updates_(ME_MAX_MARKET_UPDATES),
        run_(false), logger_("exchange_market_data_publisher.log"), incremental_socket_(logger_) {
    ASSERT(incremental_socket_.init(incremental_ip, iface, incremental_port, /*is_listening*/ false) >= 0,
           "Unable to create incremental mcast socket. error:" + std::string(std::strerror(errno)));
    snapshot_synthesizer_ = new SnapshotSynthesizer(&snapshot_md_updates_, iface, snapshot_ip, snapshot_port);
  }

  /// Main run loop for this thread - consumes market updates from the lock free queue from the matching engine, publishes them on the incremental multicast stream and forwards them to the snapshot synthesizer.
  auto MarketDataPublisher::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      for (auto market_update = outgoing_md_updates_->peek();
           outgoing_md_updates_->size() && market_update; market_update = outgoing_md_updates_->peek()) {
        TTT_MEASURE(T5_MarketDataPublisher_LFQueue_read, logger_);

        logger_.log("%:% %() % Sending seq:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), next_inc_seq_num_,
                    market_update->toString().c_str());

        START_MEASURE(Exchange_McastSocket_send);
        const auto bytes=Common::Wire::encode(MDPMarketUpdate{next_inc_seq_num_,*market_update});
        if(!incremental_socket_.send(bytes.data(),bytes.size()))break;
        END_MEASURE(Exchange_McastSocket_send, logger_);

        const auto update_copy=*market_update;
        outgoing_md_updates_->pop();
        TTT_MEASURE(T6_MarketDataPublisher_UDP_write, logger_);

        // Forward this incremental market data update the snapshot synthesizer.
        const MDPMarketUpdate snapshot_update{next_inc_seq_num_,update_copy};
        ASSERT(snapshot_md_updates_.try_push(snapshot_update),"snapshot queue full; matching state cannot be represented");

        ++next_inc_seq_num_;
      }

      // Publish to the multicast stream.
      incremental_socket_.sendAndRecv();
    }
  }
}
