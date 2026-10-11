// MEClientRequest 是内部消息，OMClientRequest 加上网络会话与序号。
// 网络传输使用 order_protocol.h 的显式编码，不直接发送结构体内存；当前请求帧为 50 字节。
// 请求只支持 NEW、整单 CANCEL、QUERY，没有部分撤单请求。

#pragma once // 防止头文件重复包含。
#include <sstream> // 引入格式化字符串流。
#include "common/types.h" // 引入common/types.h 的声明。
#include "common/lf_queue.h" // 引入common/lf_queue.h 的声明。
using namespace Common; // 使用公共业务类型名称。
namespace Exchange { // 所属命名空间。
  enum class ClientRequestType : uint8_t { // 定义事件类型及其固定编号。
    INVALID = 0, // 未初始化/非法类型。
    NEW = 1, // 提交新订单。
    CANCEL = 2, // 请求整单撤销。
    QUERY = 3 // 查询订单或账户状态。
  }; // 结束当前作用域。
  inline std::string clientRequestTypeToString(ClientRequestType type) { // 将类型转换为诊断文本。
    switch (type) { // 按事件类型选择文本。
      case ClientRequestType::NEW: // 处理对应类型。
        return "NEW"; // 返回该类型的文本名称。
      case ClientRequestType::CANCEL: // 处理对应类型。
        return "CANCEL"; // 返回该类型的文本名称。
      case ClientRequestType::QUERY: // 处理对应类型。
        return "QUERY"; // 返回该类型的文本名称。
      case ClientRequestType::INVALID: // 处理对应类型。
        return "INVALID"; // 返回该类型的文本名称。
    } // 结束当前作用域。

    return "UNKNOWN"; // 返回该类型的文本名称。
  } // 结束当前作用域。

  struct MEClientRequest { // 定义MEClientRequest 的接口和状态。
    ClientRequestType type_ = ClientRequestType::INVALID; // 请求或回报的事件类型，初始为无效值。
    ClientId client_id_ = ClientId_INVALID; // 所属客户 ID。
    TickerId ticker_id_ = TickerId_INVALID; // 交易品种 ID。
    OrderId order_id_ = OrderId_INVALID; // 客户指定的订单 ID。
    Side side_ = Side::INVALID; // 买卖方向。
    Price price_ = Price_INVALID; // 整数价格。
    Qty qty_ = Qty_INVALID; // 请求数量。
    uint64_t session_epoch_=0,request_seq_=0; // 内部携带的会话标识和请求序号。
    auto toString() const { // 生成诊断文本，不作为网络编码。
      std::stringstream ss; // 准备字符串格式化流。
      ss << "MEClientRequest" // 追加诊断字段或分隔符。
         << " [" // 追加诊断字段或分隔符。
         << "type:" << clientRequestTypeToString(type_) // 追加诊断字段或分隔符。
         << " client:" << clientIdToString(client_id_) // 追加诊断字段或分隔符。
         << " ticker:" << tickerIdToString(ticker_id_) // 追加诊断字段或分隔符。
         << " oid:" << orderIdToString(order_id_) // 追加诊断字段或分隔符。
         << " side:" << sideToString(side_) // 追加诊断字段或分隔符。
         << " qty:" << qtyToString(qty_) // 追加诊断字段或分隔符。
         << " price:" << priceToString(price_) // 追加诊断字段或分隔符。
         << "]"; // 追加诊断字段或分隔符。
      return ss.str(); // 取出完整诊断字符串。
    } // 结束当前作用域。
  }; // 结束当前作用域。
  struct OMClientRequest { // 定义OMClientRequest 的接口和状态。
    uint64_t seq_num_ = 0; // 该方向的连续网络帧序号。
    MEClientRequest me_client_request_; // 内部请求载荷。
    uint64_t session_epoch_=0; // 所属进程会话标识。
    auto toString() const { // 生成诊断文本，不作为网络编码。
      std::stringstream ss; // 准备字符串格式化流。
      ss << "OMClientRequest" // 追加诊断字段或分隔符。
         << " [" // 追加诊断字段或分隔符。
         << "seq:" << seq_num_ // 追加诊断字段或分隔符。
         << " " << me_client_request_.toString() // 追加诊断字段或分隔符。
         << "]"; // 追加诊断字段或分隔符。
      return ss.str(); // 取出完整诊断字符串。
    } // 结束当前作用域。
  }; // 结束当前作用域。
  typedef LFQueue<MEClientRequest> ClientRequestLFQueue; // 该内部消息使用的线程间队列类型。
} // 结束当前作用域。
