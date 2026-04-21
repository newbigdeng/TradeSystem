#pragma once

#include <cstdint>
#include <limits>
#include <sstream>
#include <array>

#include "common/macros.h"

namespace Common {
  /// 在整个生态系统中使用的常量，用于表示各种容器的上限。
  /// 交易工具/交易品种ID从 [0, ME_MAX_TICKERS]。
  constexpr size_t ME_MAX_TICKERS = 8;

  /// 无锁队列的最大大小，用于在组件之间传输客户端请求、客户端响应和市场更新。
  constexpr size_t ME_MAX_CLIENT_UPDATES = 256 * 1024;
  constexpr size_t ME_MAX_MARKET_UPDATES = 256 * 1024;

  /// 最大交易客户数量。
  constexpr size_t ME_MAX_NUM_CLIENTS = 256;

  /// 每个交易客户的最大订单数。
  constexpr size_t ME_MAX_ORDER_IDS = 1024 * 1024;

  /// 订单簿中的最大价格层级深度。
  constexpr size_t ME_MAX_PRICE_LEVELS = 256;

  typedef uint64_t OrderId;
  constexpr auto OrderId_INVALID = std::numeric_limits<OrderId>::max();

  inline auto orderIdToString(OrderId order_id) -> std::string {
    if (UNLIKELY(order_id == OrderId_INVALID)) {
      return "INVALID";
    }

    return std::to_string(order_id);
  }

  typedef uint32_t TickerId;
  constexpr auto TickerId_INVALID = std::numeric_limits<TickerId>::max();

  inline auto tickerIdToString(TickerId ticker_id) -> std::string {
    if (UNLIKELY(ticker_id == TickerId_INVALID)) {
      return "INVALID";
    }

    return std::to_string(ticker_id);
  }

  typedef uint32_t ClientId;
  constexpr auto ClientId_INVALID = std::numeric_limits<ClientId>::max();

  inline auto clientIdToString(ClientId client_id) -> std::string {
    if (UNLIKELY(client_id == ClientId_INVALID)) {
      return "INVALID";
    }

    return std::to_string(client_id);
  }

  typedef int64_t Price;
  constexpr auto Price_INVALID = std::numeric_limits<Price>::max();

  inline auto priceToString(Price price) -> std::string {
    if (UNLIKELY(price == Price_INVALID)) {
      return "INVALID";
    }

    return std::to_string(price);
  }

  typedef uint32_t Qty;
  constexpr auto Qty_INVALID = std::numeric_limits<Qty>::max();

  inline auto qtyToString(Qty qty) -> std::string {
    if (UNLIKELY(qty == Qty_INVALID)) {
      return "INVALID";
    }

    return std::to_string(qty);
  }

  /// 优先级表示具有相同方向和价格属性的所有订单在FIFO队列中的位置。
  typedef uint64_t Priority;
  constexpr auto Priority_INVALID = std::numeric_limits<Priority>::max();

  inline auto priorityToString(Priority priority) -> std::string {
    if (UNLIKELY(priority == Priority_INVALID)) {
      return "INVALID";
    }

    return std::to_string(priority);
  }

  enum class Side : int8_t {
    INVALID = 0,
    BUY = 1,
    SELL = -1,
    MAX = 2
  };

  inline auto sideToString(Side side) -> std::string {
    switch (side) {
      case Side::BUY:
        return "BUY";
      case Side::SELL:
        return "SELL";
      case Side::INVALID:
        return "INVALID";
      case Side::MAX:
        return "MAX";
    }

    return "UNKNOWN";
  }

  /// 将Side转换为可用于索引std::array的索引。
  inline constexpr auto sideToIndex(Side side) noexcept {
    return static_cast<size_t>(side) + 1;
  }

  /// 转换Side::BUY=1 和 Side::SELL=-1。
  inline constexpr auto sideToValue(Side side) noexcept {
    return static_cast<int>(side);
  }

  /// 交易算法类型。
  enum class AlgoType : int8_t {
    INVALID = 0,
    RANDOM = 1,
    MAKER = 2,
    TAKER = 3,
    MAX = 4
  };

  inline auto algoTypeToString(AlgoType type) -> std::string {
    switch (type) {
      case AlgoType::RANDOM:
        return "RANDOM";
      case AlgoType::MAKER:
        return "MAKER";
      case AlgoType::TAKER:
        return "TAKER";
      case AlgoType::INVALID:
        return "INVALID";
      case AlgoType::MAX:
        return "MAX";
    }

    return "UNKNOWN";
  }

  inline auto stringToAlgoType(const std::string &str) -> AlgoType {
    for (auto i = static_cast<int>(AlgoType::INVALID); i <= static_cast<int>(AlgoType::MAX); ++i) {
      const auto algo_type = static_cast<AlgoType>(i);
      if (algoTypeToString(algo_type) == str)
        return algo_type;
    }

    return AlgoType::INVALID;
  }

  /// 风险配置，包含风险管理器的风险参数限制。
  struct RiskCfg {
    Qty max_order_size_ = 0;
    Qty max_position_ = 0;
    double max_loss_ = 0;

    auto toString() const {
      std::stringstream ss;

      ss << "RiskCfg{"
         << "max-order-size:" << qtyToString(max_order_size_) << " "
         << "max-position:" << qtyToString(max_position_) << " "
         << "max-loss:" << max_loss_
         << "}";

      return ss.str();
    }
  };

  /// 顶级配置，用于配置交易引擎、交易算法和风险管理器。
  struct TradeEngineCfg {
    Qty clip_ = 0;
    double threshold_ = 0;
    RiskCfg risk_cfg_;

    auto toString() const {
      std::stringstream ss;
      ss << "TradeEngineCfg{"
         << "clip:" << qtyToString(clip_) << " "
         << "thresh:" << threshold_ << " "
         << "risk:" << risk_cfg_.toString()
         << "}";

      return ss.str();
    }
  };

  /// 从TickerId到TradeEngineCfg的哈希映射。
  typedef std::array<TradeEngineCfg, ME_MAX_TICKERS> TradeEngineCfgHashMap;
}