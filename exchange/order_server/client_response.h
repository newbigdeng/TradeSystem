#pragma once

#include <sstream>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
  /// Type of the order response sent by the exchange to the trading client.
  enum class ClientResponseType : uint8_t {
    INVALID = 0,
    ACCEPTED = 1,
    CANCELED = 2,
    FILLED = 3,
    CANCEL_REJECTED = 4,
    REJECTED = 5,
    STATE = 6
  };

  inline std::string clientResponseTypeToString(ClientResponseType type) {
    switch (type) {
      case ClientResponseType::ACCEPTED:
        return "ACCEPTED";
      case ClientResponseType::CANCELED:
        return "CANCELED";
      case ClientResponseType::FILLED:
        return "FILLED";
      case ClientResponseType::CANCEL_REJECTED:
        return "CANCEL_REJECTED";
      case ClientResponseType::REJECTED:
        return "REJECTED";
      case ClientResponseType::STATE:
        return "STATE";
      case ClientResponseType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
  }

  enum class RejectReason : uint8_t { NONE, INVALID_ID, INVALID_TYPE, INVALID_SIDE, INVALID_PRICE, INVALID_QTY, DUPLICATE_ID, CAPACITY, IDENTITY, SEQUENCE, SESSION, VERSION };

  /// Internal aligned messages. The transport uses explicit versioned big-endian encoders.

  /// Client response structure used internally by the matching engine.
  struct MEClientResponse {
    ClientResponseType type_ = ClientResponseType::INVALID;
    ClientId client_id_ = ClientId_INVALID;
    TickerId ticker_id_ = TickerId_INVALID;
    OrderId client_order_id_ = OrderId_INVALID;
    OrderId market_order_id_ = OrderId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    Qty exec_qty_ = Qty_INVALID;
    Qty leaves_qty_ = Qty_INVALID;
    RejectReason reject_reason_=RejectReason::NONE;
    uint64_t response_id_=0;
    int64_t position_=0;

    auto toString() const {
      std::stringstream ss;
      ss << "MEClientResponse"
         << " ["
         << "type:" << clientResponseTypeToString(type_)
         << " client:" << clientIdToString(client_id_)
         << " ticker:" << tickerIdToString(ticker_id_)
         << " coid:" << orderIdToString(client_order_id_)
         << " moid:" << orderIdToString(market_order_id_)
         << " side:" << sideToString(side_)
         << " exec_qty:" << qtyToString(exec_qty_)
         << " leaves_qty:" << qtyToString(leaves_qty_)
         << " reason:" << static_cast<int>(reject_reason_)
         << " price:" << priceToString(price_)
         << "]";
      return ss.str();
    }
  };

  /// Client response structure published over the network by the order server.
  struct OMClientResponse {
    uint64_t seq_num_ = 0;
    MEClientResponse me_client_response_;
    uint64_t session_epoch_=0;

    auto toString() const {
      std::stringstream ss;
      ss << "OMClientResponse"
         << " ["
         << "seq:" << seq_num_
         << " " << me_client_response_.toString()
         << "]";
      return ss.str();
    }
  };


  /// Lock free queues of matching engine client order response messages.
  typedef LFQueue<MEClientResponse> ClientResponseLFQueue;
}
