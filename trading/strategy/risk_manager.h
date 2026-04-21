#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "position_keeper.h"
#include "om_order.h"

using namespace Common;

namespace Trading {
  class OrderManager;

  /// 枚举捕获风险检查的结果 - ALLOWED表示通过了所有风险检查，其他值表示失败原因。
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

  /// 表示单个交易工具风险检查所需信息的结构。
  struct RiskInfo {
    const PositionInfo *position_info_ = nullptr;

    RiskCfg risk_cfg_;

    /// 检查风险，看我们是否被允许在指定方向上发送指定数量的订单。
    /// 将返回一个RiskCheckResult值来传达风险检查的输出。
    auto checkPreTradeRisk(Side side, Qty qty) const noexcept {
      // check order-size
      if (UNLIKELY(qty > risk_cfg_.max_order_size_))
        return RiskCheckResult::ORDER_TOO_LARGE;
      if (UNLIKELY(std::abs(position_info_->position_ + sideToValue(side) * static_cast<int32_t>(qty)) > static_cast<int32_t>(risk_cfg_.max_position_)))
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

  /// 从交易品种ID到风险信息的哈希映射。
  typedef std::array<RiskInfo, ME_MAX_TICKERS> TickerRiskInfoHashMap;

  /// 顶层风险管理类，在所有交易工具中计算和检查风险。
  class RiskManager {
  public:
    RiskManager(Common::Logger *logger, const PositionKeeper *position_keeper, const TradeEngineCfgHashMap &ticker_cfg);

    auto checkPreTradeRisk(TickerId ticker_id, Side side, Qty qty) const noexcept {
      return ticker_risk_.at(ticker_id).checkPreTradeRisk(side, qty);
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    RiskManager() = delete;

    RiskManager(const RiskManager &) = delete;

    RiskManager(const RiskManager &&) = delete;

    RiskManager &operator=(const RiskManager &) = delete;

    RiskManager &operator=(const RiskManager &&) = delete;

  private:
    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    /// Hash map container from TickerId -> RiskInfo.
    TickerRiskInfoHashMap ticker_risk_;
  };
}