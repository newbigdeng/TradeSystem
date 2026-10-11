// 一个网关对应一个客户会话。epoch 用于区分进程会话，两个方向的序号分别递增。
// 会话健康标志交给交易引擎作为发送门禁；断线后需要对账，不承诺自动恢复。

#pragma once // 防止头文件重复包含。
#include "common/order_protocol.h" // 引入common/order_protocol.h 的声明。
#include <functional> // 引入回调函数封装。
#include "common/thread_utils.h" // 引入common/thread_utils.h 的声明。
#include "common/macros.h" // 引入common/macros.h 的声明。
#include "common/tcp_server.h" // 引入common/tcp_server.h 的声明。
#include "exchange/order_server/client_request.h" // 引入exchange/order_server/client_request.h 的声明。
#include "exchange/order_server/client_response.h" // 引入exchange/order_server/client_response.h 的声明。
namespace Trading { // 所属命名空间。
  class OrderGateway { // 定义OrderGateway 的接口和状态。
  public: // 对外接口。
    OrderGateway(ClientId client_id, // 构造指定客户的网关。
                 Exchange::ClientRequestLFQueue *client_requests, // 借用出向请求队列。
                 Exchange::ClientResponseLFQueue *client_responses, // 借用入向回报队列。
                 std::string ip, const std::string &iface, int port); // 提供服务器端点和接口配置。
    ~OrderGateway() { // 销毁前先结束线程。
      stop(); // 等待网络排空或明确超时。
  } // 结束当前作用域。

    auto start() { // 连接服务器并启动网络线程。
      run_ = true; // 设置运行标志。
      ASSERT(tcp_socket_.connect(ip_, iface_, port_, false) >= 0, // 建立 TCP 主动连接并检查结果。
             "Unable to connect to ip:" + ip_ + " port:" + std::to_string(port_) + " on iface:" + iface_ + " error:" + std::string(std::strerror(errno))); // 输出地址、端口和系统错误原因。
      worker_.reset(Common::createAndStartThread(-1, "Trading/OrderGateway", [this]() { run(); })); // 由独立线程执行网关循环。
    } // 结束当前作用域。

    auto stop() -> void { // 请求停止并等待网关线程。
      stop_deadline_=Common::getMonotonicNanos()+5*NANOS_TO_SECS;run_=false; // 设置五秒排空期限，再停止正常运行。
      if(worker_ && worker_->joinable())worker_->join(); // 等待线程完成，避免析构期间仍有回调。
    } // 结束当前作用域。

    OrderGateway() = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderGateway(const OrderGateway &) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderGateway(const OrderGateway &&) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderGateway &operator=(const OrderGateway &) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    OrderGateway &operator=(const OrderGateway &&) = delete; // 禁止复制或默认构造，避免重复管理线程和连接。
    std::atomic<bool>& sessionHealthy() noexcept {return session_healthy_;} // 向交易引擎提供跨线程发送门禁。
    uint64_t sessionEpoch() const noexcept {return session_epoch_;} // 查询当前进程会话标识。
  private: // 内部状态与辅助接口。
    const ClientId client_id_; // 所属客户 ID。
    const uint64_t session_epoch_=Common::getCurrentNanos(); // 使用启动时间产生本进程 epoch；并非持久化的全局唯一号。
    std::atomic<bool> session_healthy_{true}; // 会话不确定时关闭发送门禁。
    std::string ip_; // 服务器 IP 文本。
    const std::string iface_; // 接口配置，TCP 主动连接当前不据此强制绑定源接口。
    const int port_ = 0; // 服务器端口。
    Exchange::ClientRequestLFQueue *outgoing_requests_ = nullptr; // 交易引擎生产，网关消费。
    Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr; // 网关生产，交易引擎消费。
    std::atomic<bool> run_{false}; // 工作线程运行标志。
    std::unique_ptr<std::thread> worker_; // 拥有网络线程。
    std::string time_str_; // 日志时间格式化缓冲。
    Logger logger_; // 客户独立日志。
    size_t next_outgoing_seq_num_ = 1; // 下一条出向请求序号。
    size_t next_exp_seq_num_ = 1; // 下一条期望入向回报序号。
    Common::TCPSocket tcp_socket_; // 非阻塞 TCP 连接与收发缓冲。
    uint64_t pending_requests_=0,last_receive_=0; // 待确认请求数与最近收到回报的单调时刻。
    std::atomic<Nanos> stop_deadline_{INT64_MAX}; // 停止后排空期限。
  private: // 内部状态与辅助接口。
    auto run() noexcept -> void; // 执行收发和排空循环。
    auto recvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void; // 验证、切分并投递私有回报。
  }; // 结束当前作用域。
} // 结束当前作用域。
