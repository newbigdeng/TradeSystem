#pragma once
#include <map>
#include <cmath>
#include "common/order_protocol.h"

#include "common/macros.h"
#include "common/logging.h"

#include "position_keeper.h"
#include "om_order.h"

using namespace Common;

namespace Trading {
  class OrderManager;

  /// Enumeration that captures the result of a risk check - ALLOWED means it passed all risk checks, the other values represent the failure reason.
  enum class RiskCheckResult : int8_t {
    INVALID = 0,
    ORDER_TOO_LARGE = 1,
    POSITION_TOO_LARGE = 2,
    LOSS_TOO_LARGE = 3,
    ALLOWED = 4
  };

  inline auto riskCheckResultToString(RiskCheckResult result) {
    switch (result) {
      case RiskCheckResult::INVALID:
        return "INVALID";
      case RiskCheckResult::ORDER_TOO_LARGE:
        return "ORDER_TOO_LARGE";
      case RiskCheckResult::POSITION_TOO_LARGE:
        return "POSITION_TOO_LARGE";
      case RiskCheckResult::LOSS_TOO_LARGE:
        return "LOSS_TOO_LARGE";
      case RiskCheckResult::ALLOWED:
        return "ALLOWED";
    }

    return "";
  }

  /// Structure that represents the information needed for risk checks for a single trading instrument.
  struct RiskInfo {
    const PositionInfo *position_info_ = nullptr;

    RiskCfg risk_cfg_;

    /// Check risk to see if we are allowed to send an order of the specified quantity on the specified side.
    /// Will return a RiskCheckResult value to convey the output of the risk check.
    auto checkPreTradeRisk(Side side, Qty qty) const noexcept {
      // check order-size
      if((side!=Side::BUY && side!=Side::SELL) || !qty || qty>INT32_MAX) return RiskCheckResult::INVALID;
      if (UNLIKELY(qty > risk_cfg_.max_order_size_))
        return RiskCheckResult::ORDER_TOO_LARGE;
      if (UNLIKELY(std::abs(int64_t(position_info_->position_) + sideToValue(side) * int64_t(qty)) > int64_t(risk_cfg_.max_position_)))
        return RiskCheckResult::POSITION_TOO_LARGE;
      if (UNLIKELY(position_info_->total_pnl_ < risk_cfg_.max_loss_))
        return RiskCheckResult::LOSS_TOO_LARGE;

      return RiskCheckResult::ALLOWED;
    }

    auto toString() const {
      std::stringstream ss;
      ss << "RiskInfo" << "["
         << "pos:" << position_info_->toString() << " "
         << risk_cfg_.toString()
         << "]";

      return ss.str();
    }
  };

  /// Hash map from TickerId -> RiskInfo.
  typedef std::array<RiskInfo, ME_MAX_TICKERS> TickerRiskInfoHashMap;

  /// Top level risk manager class to compute and check risk across all trading instruments.
  class RiskManager {
  public:
    RiskManager(Common::Logger *logger, const PositionKeeper *position_keeper, const TradeEngineCfgHashMap &ticker_cfg);

    auto checkPreTradeRisk(TickerId ticker_id, Side side, Qty qty) const noexcept {
      if(ticker_id>=ME_MAX_TICKERS)return RiskCheckResult::INVALID;
      const auto &risk=ticker_risk_[ticker_id];
      auto result=risk.checkPreTradeRisk(side,qty);
      if(result!=RiskCheckResult::ALLOWED)return result;
      const auto position=int64_t(risk.position_info_->position_);
      const auto buys=pending_buy_[ticker_id]+(side==Side::BUY?qty:0);
      const auto sells=pending_sell_[ticker_id]+(side==Side::SELL?qty:0);
      if(position+int64_t(buys)>int64_t(risk.risk_cfg_.max_position_) || position-int64_t(sells)<-int64_t(risk.risk_cfg_.max_position_))return RiskCheckResult::POSITION_TOO_LARGE;
      if(!std::isfinite(risk.position_info_->total_pnl_))return RiskCheckResult::LOSS_TOO_LARGE;
      return RiskCheckResult::ALLOWED;
    }

    RiskCheckResult reserve(const Exchange::MEClientRequest& r) {
      if(r.type_!=Exchange::ClientRequestType::NEW)return RiskCheckResult::ALLOWED;
      const auto key=std::make_pair(r.ticker_id_,r.order_id_);
      if(reservations_.contains(key))return RiskCheckResult::INVALID;
      const auto result=checkPreTradeRisk(r.ticker_id_,r.side_,r.qty_);
      if(result!=RiskCheckResult::ALLOWED)return result;
      reservations_.emplace(key,Reservation{r.side_,r.qty_,r.price_});
      (r.side_==Side::BUY?pending_buy_:pending_sell_)[r.ticker_id_]+=r.qty_;return result;
    }
    void rollback(const Exchange::MEClientRequest& r) {
      if(r.type_!=Exchange::ClientRequestType::NEW)return;
      const auto key=std::make_pair(r.ticker_id_,r.order_id_);auto found=reservations_.find(key);
      if(found!=reservations_.end()) {
        (found->second.side==Side::BUY?pending_buy_:pending_sell_)[r.ticker_id_]-=found->second.qty;reservations_.erase(found);
      }
    }
    bool onResponse(const Exchange::MEClientResponse& r) {
      if(r.ticker_id_>=ME_MAX_TICKERS)return false;
      auto found=reservations_.find({r.ticker_id_,r.client_order_id_});
      if(found==reservations_.end())return r.type_!=Exchange::ClientResponseType::FILLED;
      auto &reservation=found->second;auto &pending=(reservation.side==Side::BUY?pending_buy_:pending_sell_)[r.ticker_id_];
      if(r.type_==Exchange::ClientResponseType::FILLED) {
        if(r.side_!=reservation.side || !r.exec_qty_ || r.exec_qty_>reservation.qty || uint64_t(r.exec_qty_)+r.leaves_qty_!=reservation.qty)return false;
        pending-=r.exec_qty_;reservation.qty=r.leaves_qty_;if(!reservation.qty)reservations_.erase(found);
      } else if(r.type_==Exchange::ClientResponseType::CANCEL_REJECTED) {
        if(r.leaves_qty_!=reservation.qty)return false;
      } else if(r.type_==Exchange::ClientResponseType::REJECTED && r.reject_reason_==Exchange::RejectReason::DUPLICATE_ID) {
        return false; // The original economic order may still be live.
      } else if(r.type_==Exchange::ClientResponseType::CANCELED || r.type_==Exchange::ClientResponseType::REJECTED) {
        pending-=reservation.qty;reservations_.erase(found);
      }
      return true;
    }
    uint64_t pending(TickerId ticker,Side side) const {return (side==Side::BUY?pending_buy_:pending_sell_).at(ticker);}
    std::vector<Exchange::MEClientRequest> cancellations(ClientId client) const {
      std::vector<Exchange::MEClientRequest> result;
      for(const auto& [key,order]:reservations_)result.push_back({Exchange::ClientRequestType::CANCEL,client,key.first,key.second,order.side,order.price,order.qty});
      return result;
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    RiskManager() = delete;

    RiskManager(const RiskManager &) = delete;

    RiskManager(const RiskManager &&) = delete;

    RiskManager &operator=(const RiskManager &) = delete;

    RiskManager &operator=(const RiskManager &&) = delete;

  private:
    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    /// Hash map container from TickerId -> RiskInfo.
    TickerRiskInfoHashMap ticker_risk_{};
    struct Reservation {Side side;Qty qty;Price price;};
    std::map<std::pair<TickerId,OrderId>,Reservation> reservations_;
    std::array<uint64_t,ME_MAX_TICKERS> pending_buy_{},pending_sell_{};
  };
}
