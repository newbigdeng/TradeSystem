#include "me_order_book.h"

#include "matcher/matching_engine.h"

namespace Exchange {
  MEOrderBook::MEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine)
      : ticker_id_(ticker_id), matching_engine_(matching_engine), orders_at_price_pool_(ME_MAX_PRICE_LEVELS), order_pool_(ME_MAX_ORDER_IDS),
        logger_(logger) {
  }

  MEOrderBook::~MEOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                toString(false, true));

    matching_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    for (auto &itr: cid_oid_to_order_) {
      itr.clear();
    }
  }

  /// Match a new aggressive order with the provided parameters against a passive order held in the bid_itr object and generate client responses and market updates for the match.
  /// It will update the passive order (bid_itr) based on the match and possibly remove it if fully matched.
  /// It will return remaining quantity on the aggressive order in the leaves_qty parameter.
  auto MEOrderBook::match(TickerId ticker_id, ClientId client_id, Side side, OrderId client_order_id, OrderId new_market_order_id, MEOrder* itr, Qty* leaves_qty) noexcept {
    const auto order = itr;
    const auto order_qty = order->qty_;
    const auto fill_qty = std::min(*leaves_qty, order_qty);

    *leaves_qty -= fill_qty;
    order->qty_ -= fill_qty;

    client_response_ = {ClientResponseType::FILLED, client_id, ticker_id, client_order_id,
                        new_market_order_id, side, itr->price_, fill_qty, *leaves_qty};
    emitResponse(client_response_);

    client_response_ = {ClientResponseType::FILLED, order->client_id_, ticker_id, order->client_order_id_,
                        order->market_order_id_, order->side_, itr->price_, fill_qty, order->qty_};
    emitResponse(client_response_);

    market_update_ = {MarketUpdateType::TRADE, OrderId_INVALID, ticker_id, side, itr->price_, fill_qty, Priority_INVALID};
    emitMarket(market_update_);

    if (!order->qty_) {
      market_update_ = {MarketUpdateType::CANCEL, order->market_order_id_, ticker_id, order->side_,
                        order->price_, order_qty, Priority_INVALID};
      emitMarket(market_update_);

      START_MEASURE(Exchange_MEOrderBook_removeOrder);
      removeOrder(order);
      END_MEASURE(Exchange_MEOrderBook_removeOrder, (*logger_));
    } else {
      market_update_ = {MarketUpdateType::MODIFY, order->market_order_id_, ticker_id, order->side_,
                        order->price_, order->qty_, order->priority_};
      emitMarket(market_update_);
    }
  }

  /// Check if a new order with the provided attributes would match against existing passive orders on the other side of the order book.
  /// This will call the match() method to perform the match if there is a match to be made and return the quantity remaining if any on this new order.
  auto MEOrderBook::checkForMatch(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty, OrderId new_market_order_id) noexcept {
    auto leaves_qty = qty;

    if (side == Side::BUY) {
      while (leaves_qty && asks_by_price_) {
        const auto ask_itr = asks_by_price_->first_me_order_;
        if (LIKELY(price < ask_itr->price_)) {
          break;
        }

        START_MEASURE(Exchange_MEOrderBook_match);
        match(ticker_id, client_id, side, client_order_id, new_market_order_id, ask_itr, &leaves_qty);
        END_MEASURE(Exchange_MEOrderBook_match, (*logger_));
      }
    }
    if (side == Side::SELL) {
      while (leaves_qty && bids_by_price_) {
        const auto bid_itr = bids_by_price_->first_me_order_;
        if (LIKELY(price > bid_itr->price_)) {
          break;
        }

        START_MEASURE(Exchange_MEOrderBook_match);
        match(ticker_id, client_id, side, client_order_id, new_market_order_id, bid_itr, &leaves_qty);
        END_MEASURE(Exchange_MEOrderBook_match, (*logger_));
      }
    }

    return leaves_qty;
  }

  /// Create and add a new order in the order book with provided attributes.
  /// It will check to see if this new order matches an existing passive order with opposite side, and perform the matching if that is the case.
  auto MEOrderBook::add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty) noexcept -> void {
    RejectReason reason=RejectReason::NONE;
    if(client_id>=ME_MAX_NUM_CLIENTS || ticker_id!=ticker_id_ || !client_order_id || client_order_id>=ME_MAX_ORDER_IDS) reason=RejectReason::INVALID_ID;
    else if(side!=Side::BUY && side!=Side::SELL) reason=RejectReason::INVALID_SIDE;
    else if(price<=0 || price==Price_INVALID) reason=RejectReason::INVALID_PRICE;
    else if(!qty || qty==Qty_INVALID || qty>INT32_MAX) reason=RejectReason::INVALID_QTY;
    else if(used_client_order_ids_[client_id].contains(client_order_id)) reason=RejectReason::DUPLICATE_ID;
    else if(!order_pool_.available() || next_market_order_id_>=ME_MAX_ORDER_IDS || (!getOrdersAtPrice(price,side) && !orders_at_price_pool_.available())) reason=RejectReason::CAPACITY;
    if(reason!=RejectReason::NONE) {
      emitResponse({ClientResponseType::REJECTED,client_id,ticker_id,client_order_id,OrderId_INVALID,side,price,0,0,reason});return;
    }
    used_client_order_ids_[client_id].insert(client_order_id);
    const auto new_market_order_id = generateNewMarketOrderId();
    client_response_ = {ClientResponseType::ACCEPTED, client_id, ticker_id, client_order_id, new_market_order_id, side, price, 0, qty};
    emitResponse(client_response_);

    START_MEASURE(Exchange_MEOrderBook_checkForMatch);
    const auto leaves_qty = checkForMatch(client_id, client_order_id, ticker_id, side, price, qty, new_market_order_id);
    END_MEASURE(Exchange_MEOrderBook_checkForMatch, (*logger_));

    if (LIKELY(leaves_qty)) {
      const auto priority = getNextPriority(price,side);

      auto order = order_pool_.allocate(ticker_id, client_id, client_order_id, new_market_order_id, side, price, leaves_qty, priority, nullptr,
                                        nullptr);
      START_MEASURE(Exchange_MEOrderBook_addOrder);
      addOrder(order);
      END_MEASURE(Exchange_MEOrderBook_addOrder, (*logger_));

      market_update_ = {MarketUpdateType::ADD, new_market_order_id, ticker_id, side, price, leaves_qty, priority};
      emitMarket(market_update_);
    }
  }

  /// Attempt to cancel an order in the order book, issue a cancel-rejection if order does not exist.
  auto MEOrderBook::cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void {
    MEOrder *exchange_order=nullptr;
    if(client_id<ME_MAX_NUM_CLIENTS && ticker_id==ticker_id_) {
      const auto found=cid_oid_to_order_[client_id].find(order_id);
      if(found!=cid_oid_to_order_[client_id].end()) exchange_order=found->second;
    }
    const bool is_cancelable=exchange_order!=nullptr;

    if (UNLIKELY(!is_cancelable)) {
      client_response_ = {ClientResponseType::CANCEL_REJECTED, client_id, ticker_id, order_id, OrderId_INVALID,
                          Side::INVALID, Price_INVALID, 0, 0, RejectReason::INVALID_ID};
    } else {
      client_response_ = {ClientResponseType::CANCELED, client_id, ticker_id, order_id, exchange_order->market_order_id_,
                          exchange_order->side_, exchange_order->price_, Qty_INVALID, exchange_order->qty_};
      market_update_ = {MarketUpdateType::CANCEL, exchange_order->market_order_id_, ticker_id, exchange_order->side_, exchange_order->price_, 0,
                        exchange_order->priority_};

      START_MEASURE(Exchange_MEOrderBook_removeOrder);
      removeOrder(exchange_order);
      END_MEASURE(Exchange_MEOrderBook_removeOrder, (*logger_));

      emitMarket(market_update_);
    }

    emitResponse(client_response_);
  }

  auto MEOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;
    std::string time_str;

    auto printer = [&](std::stringstream &ss, MEOrdersAtPrice *itr, Side side, Price &last_price, bool sanity_check) {
      char buf[4096];
      Qty qty = 0;
      size_t num_orders = 0;

      for (auto o_itr = itr->first_me_order_;; o_itr = o_itr->next_order_) {
        qty += o_itr->qty_;
        ++num_orders;
        if (o_itr->next_order_ == itr->first_me_order_)
          break;
      }
      sprintf(buf, " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
              priceToString(itr->price_).c_str(), priceToString(itr->prev_entry_->price_).c_str(), priceToString(itr->next_entry_->price_).c_str(),
              priceToString(itr->price_).c_str(), qtyToString(qty).c_str(), std::to_string(num_orders).c_str());
      ss << buf;
      for (auto o_itr = itr->first_me_order_;; o_itr = o_itr->next_order_) {
        if (detailed) {
          sprintf(buf, "[oid:%s q:%s p:%s n:%s] ",
                  orderIdToString(o_itr->market_order_id_).c_str(), qtyToString(o_itr->qty_).c_str(),
                  orderIdToString(o_itr->prev_order_ ? o_itr->prev_order_->market_order_id_ : OrderId_INVALID).c_str(),
                  orderIdToString(o_itr->next_order_ ? o_itr->next_order_->market_order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o_itr->next_order_ == itr->first_me_order_)
          break;
      }

      ss << std::endl;

      if (sanity_check) {
        if ((side == Side::SELL && last_price >= itr->price_) || (side == Side::BUY && last_price <= itr->price_)) {
          FATAL("Bids/Asks not sorted by ascending/descending prices last:" + priceToString(last_price) + " itr:" + itr->toString());
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

namespace Exchange {
void MEOrderBook::emitResponse(const MEClientResponse& response) {
  if(response.client_id_<ME_MAX_NUM_CLIENTS && response.type_!=ClientResponseType::REJECTED && response.type_!=ClientResponseType::CANCEL_REJECTED && response.type_!=ClientResponseType::STATE)
    last_order_response_[response.client_id_][response.client_order_id_]=response;
  if(response_sink_) response_sink_(response);
  else if(matching_engine_) matching_engine_->sendClientResponse(&response);
}
void MEOrderBook::emitMarket(const MEMarketUpdate& update) {
  if(market_sink_) market_sink_(update);
  else if(matching_engine_) matching_engine_->sendMarketUpdate(&update);
}
}

namespace Exchange {
void MEOrderBook::query(ClientId client_id,OrderId order_id) {
  if(client_id>=ME_MAX_NUM_CLIENTS)return;
  if(!order_id) {emitResponse({ClientResponseType::STATE,client_id,ticker_id_,0,OrderId_INVALID,Side::INVALID,Price_INVALID,0,0});return;}
  const auto found=last_order_response_[client_id].find(order_id);
  if(found==last_order_response_[client_id].end()) {
    emitResponse({ClientResponseType::REJECTED,client_id,ticker_id_,order_id,OrderId_INVALID,Side::INVALID,Price_INVALID,0,0,RejectReason::INVALID_ID});return;
  }
  auto response=found->second;
  if(response.type_==ClientResponseType::CANCELED)response.leaves_qty_=0;
  response.type_=ClientResponseType::STATE;response.exec_qty_=0;
  emitResponse(response);
}
}
