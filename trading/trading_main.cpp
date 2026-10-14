// 启动时读取客户端 JSON，再装配策略、TCP 网关和两路组播接收器。
// 先等待行情恢复，再按策略运行；订单会话失效时停止发送并保存对账状态。
// 停机先禁止新请求，再停行情、排空网关、停交易引擎，最后写账户检查点。

#include <csignal>
#include <cmath>
#include <filesystem>
#include <memory>
#include "strategy/trade_engine.h"
#include "order_gw/order_gateway.h"
#include "market_data/market_data_consumer.h"
#include "common/critical_journal.h"
#include "common/startup_options.h"

// 信号处理器只设置退出标志，不执行日志和复杂清理。
namespace {
volatile std::sig_atomic_t stopping = 0;

void signalHandler(int) {
  stopping = 1;
}

// 兼容原有模拟负载命令行；网络端点始终来自客户端 JSON。
void applyLegacyArguments(Common::ClientConfig& config, int argc, char** argv, int first) {
  const auto remaining = argc - first;
  if (!remaining)
    return;
  if (remaining < 2 || (remaining - 2) % 5 || (remaining - 2) / 5 > int(Common::ME_MAX_TICKERS))
    throw Common::ConfigError(
        "用法：trading_main [--config 文件] [--check-config] "
        "[CLIENT_ID ALGO [CLIP THRESH MAX_ORDER_SIZE MAX_POSITION MAX_LOSS] ...]");
  config.client_id =
      Common::unsignedSetting(argv[first], Common::ME_MAX_NUM_CLIENTS - 1, "CLIENT_ID");
  config.strategy.type = argv[first + 1];
  for (int i = first + 2; i < argc; i += 5) {
    const auto ticker = static_cast<uint32_t>((i - first - 2) / 5);
    Common::StrategyParameters parameters;
    parameters.clip = Common::unsignedSetting(argv[i], INT32_MAX, "CLIP");
    parameters.threshold = Common::floatingSetting(argv[i + 1], "THRESH");
    parameters.risk.max_order_size =
        Common::unsignedSetting(argv[i + 2], INT32_MAX, "MAX_ORDER_SIZE");
    parameters.risk.max_position = Common::unsignedSetting(argv[i + 3], INT32_MAX, "MAX_POSITION");
    parameters.risk.max_loss = Common::floatingSetting(argv[i + 4], "MAX_LOSS");
    auto found = std::find_if(config.strategy.tickers.begin(), config.strategy.tickers.end(),
                              [ticker](const auto& value) { return value.ticker_id == ticker; });
    if (found == config.strategy.tickers.end())
      config.strategy.tickers.push_back({ticker, parameters});
    else
      found->parameters = parameters;
  }
}
}

// 组装并运行交易客户端。
int main(int argc, char** argv) {
  Common::ClientConfig startup;
  Common::RuntimeConfig runtime;
  try {
    const auto options = Common::startupOptions(argc, argv, "client.json");
    startup = Common::loadClientConfig(options.config_path);
    applyLegacyArguments(startup, argc, argv, options.first_positional);
    runtime = startup.runtimeConfig();
    runtime.run_seconds =
        Common::environmentSetting("TRADE_RUN_SECONDS", runtime.run_seconds, 86400);
    runtime.random_orders =
        Common::environmentSetting("TRADE_RANDOM_ORDERS", runtime.random_orders, 900000);
    runtime.random_delay_us =
        Common::environmentSetting("TRADE_RANDOM_DELAY_US", runtime.random_delay_us, 1000000);
    startup.runtime = runtime;
    Common::validateClientConfig(startup);
    if (options.check_only) {
      std::cout << "客户端配置有效：" << options.config_path << " client:" << startup.client_id
                << " strategy:" << Common::algoTypeToString(startup.strategyType()) << '\n';
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "客户端启动配置错误：" << error.what() << '\n';
    return 2;
  }

  const auto client = Common::ClientId(startup.client_id);
  const auto algo = startup.strategyType();
  const auto config = startup.tickerConfig();
  const std::string checkpoint = "trading_account_" + std::to_string(client) + ".journal";
  ASSERT(!std::filesystem::exists(checkpoint),
         "previous trading checkpoint requires reconciliation or an explicit simulation reset: " +
             checkpoint);

  // 注册退出信号，按客户 ID 固定随机种子。
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  std::srand(client);
  Common::Logger logger("trading_main_" + std::to_string(client) + ".log");  // 创建主线程日志。

  // 构造三条固定容量队列；大缓冲仍在堆上。
  Exchange::ClientRequestLFQueue requests(Common::QueueCapacity<Common::ME_MAX_CLIENT_UPDATES>{});
  Exchange::ClientResponseLFQueue responses(Common::QueueCapacity<Common::ME_MAX_CLIENT_UPDATES>{});
  Exchange::MEMarketUpdateLFQueue updates(Common::QueueCapacity<Common::ME_MAX_MARKET_UPDATES>{});

  // 构造交易引擎并连接三条队列。
  auto engine =
      std::make_unique<Trading::TradeEngine>(client, algo, config, &requests, &responses, &updates);

  // TCP 远端地址来自配置；客户端的源地址仍由系统路由选择。
  const auto& market = startup.market_data;
  auto gateway = std::make_unique<Trading::OrderGateway>(
      client, &requests, &responses, startup.tcp.address, market.interface, startup.tcp.port);
  engine->setOrderSession(&gateway->sessionHealthy());  // 把订单会话健康状态接入发送门禁。

  // 在配置的网卡订阅两路组播，并共享恢复代次。
  auto consumer = std::make_unique<Trading::MarketDataConsumer>(
      client, &updates, market.interface, market.snapshot.address, market.snapshot.port,
      market.incremental.address, market.incremental.port, &engine->marketGeneration());
  engine->start();
  gateway->start();
  consumer->start();                         // 接收器负责重建快照，策略在行情就绪前保持关闭。
  const auto seconds = runtime.run_seconds;  // 0 表示不设时间截止。

  // 用单调时钟计算运行期限。
  const auto deadline = seconds ? std::chrono::steady_clock::now() + std::chrono::seconds(seconds)
                                : std::chrono::steady_clock::time_point::max();

  // 退出信号、时间期限或会话失效都会结束运行。
  auto active = [&] {
    return !stopping && std::chrono::steady_clock::now() < deadline &&
           gateway->sessionHealthy().load();
  };

  // 首次等待完整行情恢复最多 75 秒。
  const auto initial_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(75);

  // 恢复前暂停策略启动，周期性检查信任状态。
  while (active() && !engine->marketReady() && std::chrono::steady_clock::now() < initial_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (algo == Common::AlgoType::RANDOM) {  // RANDOM 模式由主线程生成模拟请求。
    // 准备订单 ID 和已成功发送请求集合。
    Common::OrderId next_order = client * 1000;
    std::vector<Exchange::MEClientRequest> sent;

    // 为每个品种生成固定基准价。
    std::array<Common::Price, Common::ME_MAX_TICKERS> base{};
    for (auto& price : base)
      price = 100 + std::rand() % 100;

    // 启动前已校验模拟请求数量和间隔。
    const auto count = runtime.random_orders, delay = runtime.random_delay_us;
    for (uint64_t i = 0; i < count && active(); ++i) {  // 按数量与运行条件生成请求。
      const Common::TickerId ticker = std::rand() % Common::ME_MAX_TICKERS;  // 随机选取品种。

      // 构造带方向、价格和数量的新单。
      const Exchange::MEClientRequest request{
          Exchange::ClientRequestType::NEW,
          client,
          ticker,
          next_order++,
          std::rand() % 2 ? Common::Side::BUY : Common::Side::SELL,
          base[ticker] + 1 + std::rand() % 10,
          Common::Qty(2 + std::rand() % 100)};

      // 通过交易引擎风控和发送门禁后才记入已发送集合。
      if (engine->sendClientRequest(&request))
        sent.push_back(request);
      std::this_thread::sleep_for(std::chrono::microseconds(delay));  // 控制请求节奏。

      // 随机选择已发送订单尝试整单撤销，已成交订单可能拒撤。
      if (!sent.empty()) {
        auto cancel = sent[std::rand() % sent.size()];
        cancel.type_ = Exchange::ClientRequestType::CANCEL;
        engine->sendClientRequest(&cancel);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(delay));  // 撤单后再次等待，控制节奏。
    }

    if (!seconds)
      stopping = 1;  // 未配置运行期限时，生成完成即请求结束。
  }

  // 其余运行时间周期性等待退出条件。
  while (active())
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // 先禁止新请求，再停行情、排空网关、停止交易引擎。
  engine->quiesce();
  consumer->stop();
  gateway->stop();
  engine->stop();

  // 写入账户检查点及会话是否仍可确认。
  Common::CriticalJournal account(checkpoint);
  engine->writeCheckpoint(account, gateway->sessionEpoch(), gateway->sessionHealthy().load());
  logger.log("TRADING STOPPED request_queue:% response_queue:% md_queue:% session_healthy:%\n",
             requests.size(), responses.size(), updates.size(), gateway->sessionHealthy().load());
  return gateway->sessionHealthy().load() ? 0 : 3;  // 会话正常返回 0，不确定状态返回 3。
}
