// 私有订单回报与公开行情是两套类型：FILLED 表示本次成交，CANCELED 表示主动撤单成功。
// exec_qty_ 是本次成交量；leaves_qty_ 的含义随回报类型解释，CANCELED 携带撤销时的剩余量。
// 网络帧由 order_protocol.h 显式编码，不能按 sizeof(MEClientResponse) 收发；当前回报帧为 79 字节。

#pragma once // 防止头文件重复包含。
#include <sstream> // 引入格式化字符串流。
#include "common/types.h" // 引入common/types.h 的声明。
#include "common/lf_queue.h" // 引入common/lf_queue.h 的声明。
using namespace Common; // 使用公共业务类型名称。
namespace Exchange { // 所属命名空间。
  enum class ClientResponseType : uint8_t { // 定义事件类型及其固定编号。
    INVALID = 0, // 未初始化/非法类型。
    ACCEPTED = 1, // 新单已被接受，未必全部成交。
    CANCELED = 2, // 主动撤单成功。
    FILLED = 3, // 本次成交回报，可为部分或全部成交。
    CANCEL_REJECTED = 4, // 撤单请求被拒绝。
    REJECTED = 5, // 请求被拒绝。
    STATE = 6 // 查询状态回报。
  }; // 结束当前作用域。
  inline std::string clientResponseTypeToString(ClientResponseType type) { // 将类型转换为诊断文本。
    switch (type) { // 按事件类型选择文本。
      case ClientResponseType::ACCEPTED: // 处理对应类型。
        return "ACCEPTED"; // 返回该类型的文本名称。
      case ClientResponseType::CANCELED: // 处理对应类型。
        return "CANCELED"; // 返回该类型的文本名称。
      case ClientResponseType::FILLED: // 处理对应类型。
        return "FILLED"; // 返回该类型的文本名称。
      case ClientResponseType::CANCEL_REJECTED: // 处理对应类型。
        return "CANCEL_REJECTED"; // 返回该类型的文本名称。
      case ClientResponseType::REJECTED: // 处理对应类型。
        return "REJECTED"; // 返回该类型的文本名称。
      case ClientResponseType::STATE: // 处理对应类型。
        return "STATE"; // 返回该类型的文本名称。
      case ClientResponseType::INVALID: // 处理对应类型。
        return "INVALID"; // 返回该类型的文本名称。
    } // 结束当前作用域。

    return "UNKNOWN"; // 返回该类型的文本名称。
  } // 结束当前作用域。

  enum class RejectReason : uint8_t { NONE, INVALID_ID, INVALID_TYPE, INVALID_SIDE, INVALID_PRICE, INVALID_QTY, DUPLICATE_ID, CAPACITY, IDENTITY, SEQUENCE, SESSION, VERSION }; // 协议、身份、参数或容量等拒绝原因；不是公开行情的成交原因。
  struct MEClientResponse { // 定义MEClientResponse 的接口和状态。
    ClientResponseType type_ = ClientResponseType::INVALID; // 请求或回报的事件类型，初始为无效值。
    ClientId client_id_ = ClientId_INVALID; // 所属客户 ID。
    TickerId ticker_id_ = TickerId_INVALID; // 交易品种 ID。
    OrderId client_order_id_ = OrderId_INVALID; // 客户侧订单 ID。
    OrderId market_order_id_ = OrderId_INVALID; // 交易所生成的市场订单 ID。
    Side side_ = Side::INVALID; // 买卖方向。
    Price price_ = Price_INVALID; // 整数价格。
    Qty exec_qty_ = Qty_INVALID; // 本次成交数量，非适用回报可使用哨兵值。
    Qty leaves_qty_ = Qty_INVALID; // 该回报描述的剩余数量；撤单成功时表示被撤余量。
    RejectReason reject_reason_=RejectReason::NONE; // 明确的拒绝原因。
    uint64_t response_id_=0; // 业务回报唯一编号，用于重复检测。
    int64_t position_=0; // 状态查询返回的持仓。
    auto toString() const { // 生成诊断文本，不作为网络编码。
      std::stringstream ss; // 准备字符串格式化流。
      ss << "MEClientResponse" // 追加诊断字段或分隔符。
         << " [" // 追加诊断字段或分隔符。
         << "type:" << clientResponseTypeToString(type_) // 追加诊断字段或分隔符。
         << " client:" << clientIdToString(client_id_) // 追加诊断字段或分隔符。
         << " ticker:" << tickerIdToString(ticker_id_) // 追加诊断字段或分隔符。
         << " coid:" << orderIdToString(client_order_id_) // 追加诊断字段或分隔符。
         << " moid:" << orderIdToString(market_order_id_) // 追加诊断字段或分隔符。
         << " side:" << sideToString(side_) // 追加诊断字段或分隔符。
         << " exec_qty:" << qtyToString(exec_qty_) // 追加诊断字段或分隔符。
         << " leaves_qty:" << qtyToString(leaves_qty_) // 追加诊断字段或分隔符。
         << " reason:" << static_cast<int>(reject_reason_) // 追加诊断字段或分隔符。
         << " price:" << priceToString(price_) // 追加诊断字段或分隔符。
         << "]"; // 追加诊断字段或分隔符。
      return ss.str(); // 取出完整诊断字符串。
    } // 结束当前作用域。
  }; // 结束当前作用域。
  struct OMClientResponse { // 定义OMClientResponse 的接口和状态。
    uint64_t seq_num_ = 0; // 该方向的连续网络帧序号。
    MEClientResponse me_client_response_; // 内部回报载荷。
    uint64_t session_epoch_=0; // 所属进程会话标识。
    auto toString() const { // 生成诊断文本，不作为网络编码。
      std::stringstream ss; // 准备字符串格式化流。
      ss << "OMClientResponse" // 追加诊断字段或分隔符。
         << " [" // 追加诊断字段或分隔符。
         << "seq:" << seq_num_ // 追加诊断字段或分隔符。
         << " " << me_client_response_.toString() // 追加诊断字段或分隔符。
         << "]"; // 追加诊断字段或分隔符。
      return ss.str(); // 取出完整诊断字符串。
    } // 结束当前作用域。
  }; // 结束当前作用域。
  typedef LFQueue<MEClientResponse> ClientResponseLFQueue; // 该内部消息使用的线程间队列类型。
} // 结束当前作用域。
