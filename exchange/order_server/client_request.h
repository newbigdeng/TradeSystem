#pragma once

#include <sstream>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
  /// 由交易客户端发送给交易所的订单请求类型。
  enum class ClientRequestType : uint8_t {
    INVALID = 0,
    NEW = 1,
    CANCEL = 2
  };

  inline std::string clientRequestTypeToString(ClientRequestType type) {
    switch (type) {
      case ClientRequestType::NEW:
        return "NEW";
      case ClientRequestType::CANCEL:
        return "CANCEL";
      case ClientRequestType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
  }

  /// 这些结构通过网络传输，所以二进制结构被压缩以去除系统相关的额外填充。
#pragma pack(push, 1)

  /// 匹配引擎内部使用的客户端请求结构。
  struct MEClientRequest {
    ClientRequestType type_ = ClientRequestType::INVALID;

    ClientId client_id_ = ClientId_INVALID;
    TickerId ticker_id_ = TickerId_INVALID;
    OrderId order_id_ = OrderId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    Qty qty_ = Qty_INVALID;

    auto toString() const {
      std::stringstream ss;
      ss << "MEClientRequest"
         << " ["
         << "type:" << clientRequestTypeToString(type_)
         << " client:" << clientIdToString(client_id_)
         << " ticker:" << tickerIdToString(ticker_id_)
         << " oid:" << orderIdToString(order_id_)
         << " side:" << sideToString(side_)
         << " qty:" << qtyToString(qty_)
         << " price:" << priceToString(price_)
         << "]";
      return ss.str();
    }
  };

  /// 订单网关客户端通过网络发布的客户端请求结构。
  struct OMClientRequest {
    size_t seq_num_ = 0;
    MEClientRequest me_client_request_;

    auto toString() const {
      std::stringstream ss;
      ss << "OMClientRequest"
         << " ["
         << "seq:" << seq_num_
         << " " << me_client_request_.toString()
         << "]";
      return ss.str();
    }
  };

#pragma pack(pop) // 撤消后续的压缩二进制结构指令。

  /// 匹配引擎客户端订单请求消息的无锁队列。
  typedef LFQueue<MEClientRequest> ClientRequestLFQueue;
}