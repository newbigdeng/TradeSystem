#include "market_order_book.h"

#include "trade_engine.h"

namespace Trading {
  MarketOrderBook::MarketOrderBook(TickerId ticker_id, Logger *logger)
      : ticker_id_(ticker_id), orders_at_price_pool_(ME_MAX_PRICE_LEVELS), order_pool_(ME_MAX_ORDER_IDS), logger_(logger) {
  }

  MarketOrderBook::~MarketOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_), toString(false, true));

    trade_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    oid_to_order_.fill(nullptr);
  }

  /// Process market data update and update the limit order book.
  auto MarketOrderBook::onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> bool {
    if(market_update->ticker_id_!=ticker_id_) return false;
    const auto type=market_update->type_;
    if(type==Exchange::MarketUpdateType::ADD || type==Exchange::MarketUpdateType::MODIFY || type==Exchange::MarketUpdateType::CANCEL) {
      if(!market_update->order_id_ || market_update->order_id_>=ME_MAX_ORDER_IDS) return false;
      const auto *existing=oid_to_order_[market_update->order_id_];
      if(type==Exchange::MarketUpdateType::ADD) {
        if(existing || !order_pool_.available() || (market_update->side_!=Side::BUY && market_update->side_!=Side::SELL) || market_update->price_<=0 || market_update->price_==Price_INVALID || !market_update->qty_ || market_update->qty_==Qty_INVALID || (!getOrdersAtPrice(market_update->price_,market_update->side_) && !orders_at_price_pool_.available())) return false;
      } else if(!existing || existing->side_!=market_update->side_ || existing->price_!=market_update->price_ || (type==Exchange::MarketUpdateType::MODIFY && (!market_update->qty_ || market_update->qty_==Qty_INVALID))) return false;
    }
    switch (market_update->type_) {
      case Exchange::MarketUpdateType::ADD: {
        auto order = order_pool_.allocate(market_update->order_id_, market_update->side_, market_update->price_,
                                          market_update->qty_, market_update->priority_, nullptr, nullptr);
        START_MEASURE(Trading_MarketOrderBook_addOrder);
        addOrder(order);
        END_MEASURE(Trading_MarketOrderBook_addOrder, (*logger_));
      }
        break;
      case Exchange::MarketUpdateType::MODIFY: {
        auto order = oid_to_order_.at(market_update->order_id_);
        order->qty_ = market_update->qty_;
      }
        break;
      case Exchange::MarketUpdateType::CANCEL: {
        auto order = oid_to_order_.at(market_update->order_id_);
        START_MEASURE(Trading_MarketOrderBook_removeOrder);
        removeOrder(order);
        END_MEASURE(Trading_MarketOrderBook_removeOrder, (*logger_));
      }
        break;
      case Exchange::MarketUpdateType::TRADE: {
        if(trade_engine_) trade_engine_->onTradeUpdate(market_update, this);
        return true;
      }
        break;
      case Exchange::MarketUpdateType::CLEAR: { // Clear the full limit order book and deallocate MarketOrdersAtPrice and MarketOrder objects.
        for (auto &order: oid_to_order_) {
          if (order)
            order_pool_.deallocate(order);
        }
        oid_to_order_.fill(nullptr);
        live_order_ids_.clear();

        for(const auto &[key,level]:price_orders_at_price_) { (void)key;orders_at_price_pool_.deallocate(level); }
        price_orders_at_price_.clear();
        bids_by_price_ = asks_by_price_ = nullptr;
      }
        break;
      case Exchange::MarketUpdateType::RECOVERY_COMMIT:
      case Exchange::MarketUpdateType::INVALID:
      case Exchange::MarketUpdateType::SNAPSHOT_START:
      case Exchange::MarketUpdateType::SNAPSHOT_END:
        break;
    }

    START_MEASURE(Trading_MarketOrderBook_updateBBO);
    updateBBO(true, true);
    END_MEASURE(Trading_MarketOrderBook_updateBBO, (*logger_));

    logger_->log("%:% %() % % %", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_), market_update->toString(), bbo_.toString());

    if(trade_engine_) trade_engine_->onOrderBookUpdate(market_update->ticker_id_, market_update->price_, market_update->side_, this);
    return true;
  }

  auto MarketOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;
    std::string time_str;

    auto printer = [&](std::stringstream &ss, MarketOrdersAtPrice *itr, Side side, Price &last_price,
                       bool sanity_check) {
      char buf[4096];
      Qty qty = 0;
      size_t num_orders = 0;

      for (auto o_itr = itr->first_mkt_order_;; o_itr = o_itr->next_order_) {
        qty += o_itr->qty_;
        ++num_orders;
        if (o_itr->next_order_ == itr->first_mkt_order_)
          break;
      }
      sprintf(buf, " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
              priceToString(itr->price_).c_str(), priceToString(itr->prev_entry_->price_).c_str(),
              priceToString(itr->next_entry_->price_).c_str(),
              priceToString(itr->price_).c_str(), qtyToString(qty).c_str(), std::to_string(num_orders).c_str());
      ss << buf;
      for (auto o_itr = itr->first_mkt_order_;; o_itr = o_itr->next_order_) {
        if (detailed) {
          sprintf(buf, "[oid:%s q:%s p:%s n:%s] ",
                  orderIdToString(o_itr->order_id_).c_str(), qtyToString(o_itr->qty_).c_str(),
                  orderIdToString(o_itr->prev_order_ ? o_itr->prev_order_->order_id_ : OrderId_INVALID).c_str(),
                  orderIdToString(o_itr->next_order_ ? o_itr->next_order_->order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o_itr->next_order_ == itr->first_mkt_order_)
          break;
      }

      ss << std::endl;

      if (sanity_check) {
        if ((side == Side::SELL && last_price >= itr->price_) || (side == Side::BUY && last_price <= itr->price_)) {
          FATAL("Bids/Asks not sorted by ascending/descending prices last:" + priceToString(last_price) + " itr:" +
                itr->toString());
        }
        last_price = itr->price_;
      }
    };

    ss << "Ticker:" << tickerIdToString(ticker_id_) << std::endl;
    {
      auto ask_itr = asks_by_price_;
      auto last_ask_price = std::numeric_limits<Price>::min();
      for (size_t count = 0; ask_itr; ++count) {
        ss << "ASKS L:" << count << " => ";
        auto next_ask_itr = (ask_itr->next_entry_ == asks_by_price_ ? nullptr : ask_itr->next_entry_);
        printer(ss, ask_itr, Side::SELL, last_ask_price, validity_check);
        ask_itr = next_ask_itr;
      }
    }

    ss << std::endl << "                          X" << std::endl << std::endl;

    {
      auto bid_itr = bids_by_price_;
      auto last_bid_price = std::numeric_limits<Price>::max();
      for (size_t count = 0; bid_itr; ++count) {
        ss << "BIDS L:" << count << " => ";
        auto next_bid_itr = (bid_itr->next_entry_ == bids_by_price_ ? nullptr : bid_itr->next_entry_);
        printer(ss, bid_itr, Side::BUY, last_bid_price, validity_check);
        bid_itr = next_bid_itr;
      }
    }

    return ss.str();
  }
}
