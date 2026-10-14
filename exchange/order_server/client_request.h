// MEClientRequest 是内部消息，OMClientRequest 加上网络会话与序号。
// 网络传输使用 order_protocol.h 的显式编码，不直接发送结构体内存；当前请求帧为 50 字节。
// 请求只支持 NEW、整单 CANCEL、QUERY，没有部分撤单请求。

#pragma once
#include <sstream>
#include "common/types.h"
#include "common/lf_queue.h"
using namespace Common;

namespace Exchange {
enum class ClientRequestType : uint8_t {  // 定义事件类型及其固定编号。
  INVALID = 0,                            // 未初始化/非法类型。
  NEW = 1,                                // 提交新订单。
  CANCEL = 2,                             // 请求整单撤销。
  QUERY = 3                               // 查询订单或账户状态。
};

// 将类型转换为诊断文本。
inline std::string clientRequestTypeToString(ClientRequestType type) {
  switch (type) {  // 按事件类型选择文本。
    case ClientRequestType::NEW:
      return "NEW";
    case ClientRequestType::CANCEL:
      return "CANCEL";
    case ClientRequestType::QUERY:
      return "QUERY";
    case ClientRequestType::INVALID:
      return "INVALID";
  }

  return "UNKNOWN";
}

struct MEClientRequest {
  ClientRequestType type_ = ClientRequestType::INVALID;  // 请求或回报的事件类型，初始为无效值。
  ClientId client_id_ = ClientId_INVALID;                // 所属客户 ID。
  TickerId ticker_id_ = TickerId_INVALID;                // 交易品种 ID。
  OrderId order_id_ = OrderId_INVALID;                   // 客户指定的订单 ID。
  Side side_ = Side::INVALID;                            // 买卖方向。
  Price price_ = Price_INVALID;                          // 整数价格。
  Qty qty_ = Qty_INVALID;                                // 请求数量。
  uint64_t session_epoch_ = 0, request_seq_ = 0;         // 内部携带的会话标识和请求序号。

  // 生成诊断文本，不作为网络编码。
  auto toString() const {
    std::stringstream ss;
    ss << "MEClientRequest"
       << " ["
       << "type:" << clientRequestTypeToString(type_) << " client:" << clientIdToString(client_id_)
       << " ticker:" << tickerIdToString(ticker_id_) << " oid:" << orderIdToString(order_id_)
       << " side:" << sideToString(side_) << " qty:" << qtyToString(qty_)
       << " price:" << priceToString(price_) << "]";
    return ss.str();
  }
};

struct OMClientRequest {
  uint64_t seq_num_ = 0;               // 该方向的连续网络帧序号。
  MEClientRequest me_client_request_;  // 内部请求载荷。
  uint64_t session_epoch_ = 0;         // 所属进程会话标识。

  // 生成诊断文本，不作为网络编码。
  auto toString() const {
    std::stringstream ss;
    ss << "OMClientRequest"
       << " ["
       << "seq:" << seq_num_ << " " << me_client_request_.toString() << "]";
    return ss.str();
  }
};

typedef LFQueue<MEClientRequest> ClientRequestLFQueue;  // 该内部消息使用的线程间队列类型。
}
