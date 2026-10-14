// 私有订单回报与公开行情是两套类型：FILLED 表示本次成交，CANCELED 表示主动撤单成功。
// exec_qty_ 是本次成交量；leaves_qty_ 的含义随回报类型解释，CANCELED 携带撤销时的剩余量。
// 网络帧由 order_protocol.h 显式编码，不能按 sizeof(MEClientResponse) 收发；当前回报帧为 79 字节。

#pragma once
#include <sstream>
#include "common/types.h"
#include "common/lf_queue.h"
using namespace Common;

namespace Exchange {
enum class ClientResponseType : uint8_t {  // 定义事件类型及其固定编号。
  INVALID = 0,                             // 未初始化/非法类型。
  ACCEPTED = 1,                            // 新单已被接受，未必全部成交。
  CANCELED = 2,                            // 主动撤单成功。
  FILLED = 3,                              // 本次成交回报，可为部分或全部成交。
  CANCEL_REJECTED = 4,                     // 撤单请求被拒绝。
  REJECTED = 5,                            // 请求被拒绝。
  STATE = 6                                // 查询状态回报。
};

// 将类型转换为诊断文本。
inline std::string clientResponseTypeToString(ClientResponseType type) {
  switch (type) {  // 按事件类型选择文本。
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

// 协议、身份、参数或容量等拒绝原因；不是公开行情的成交原因。
enum class RejectReason : uint8_t {
  NONE,
  INVALID_ID,
  INVALID_TYPE,
  INVALID_SIDE,
  INVALID_PRICE,
  INVALID_QTY,
  DUPLICATE_ID,
  CAPACITY,
  IDENTITY,
  SEQUENCE,
  SESSION,
  VERSION
};

struct MEClientResponse {
  ClientResponseType type_ = ClientResponseType::INVALID;  // 请求或回报的事件类型，初始为无效值。
  ClientId client_id_ = ClientId_INVALID;                  // 所属客户 ID。
  TickerId ticker_id_ = TickerId_INVALID;                  // 交易品种 ID。
  OrderId client_order_id_ = OrderId_INVALID;              // 客户侧订单 ID。
  OrderId market_order_id_ = OrderId_INVALID;              // 交易所生成的市场订单 ID。
  Side side_ = Side::INVALID;                              // 买卖方向。
  Price price_ = Price_INVALID;                            // 整数价格。
  Qty exec_qty_ = Qty_INVALID;                             // 本次成交数量，非适用回报可使用哨兵值。
  Qty leaves_qty_ = Qty_INVALID;  // 该回报描述的剩余数量；撤单成功时表示被撤余量。
  RejectReason reject_reason_ = RejectReason::NONE;  // 明确的拒绝原因。
  uint64_t response_id_ = 0;                         // 业务回报唯一编号，用于重复检测。
  int64_t position_ = 0;                             // 状态查询返回的持仓。

  // 生成诊断文本，不作为网络编码。
  auto toString() const {
    std::stringstream ss;
    ss << "MEClientResponse"
       << " ["
       << "type:" << clientResponseTypeToString(type_) << " client:" << clientIdToString(client_id_)
       << " ticker:" << tickerIdToString(ticker_id_)
       << " coid:" << orderIdToString(client_order_id_)
       << " moid:" << orderIdToString(market_order_id_) << " side:" << sideToString(side_)
       << " exec_qty:" << qtyToString(exec_qty_) << " leaves_qty:" << qtyToString(leaves_qty_)
       << " reason:" << static_cast<int>(reject_reason_) << " price:" << priceToString(price_)
       << "]";
    return ss.str();
  }
};

struct OMClientResponse {
  uint64_t seq_num_ = 0;                 // 该方向的连续网络帧序号。
  MEClientResponse me_client_response_;  // 内部回报载荷。
  uint64_t session_epoch_ = 0;           // 所属进程会话标识。

  // 生成诊断文本，不作为网络编码。
  auto toString() const {
    std::stringstream ss;
    ss << "OMClientResponse"
       << " ["
       << "seq:" << seq_num_ << " " << me_client_response_.toString() << "]";
    return ss.str();
  }
};

typedef LFQueue<MEClientResponse> ClientResponseLFQueue;  // 该内部消息使用的线程间队列类型。
}
