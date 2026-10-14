#include <csignal>
#include <memory>
#include "common/startup_options.h"
#include "matcher/matching_engine.h"
#include "market_data/market_data_publisher.h"
#include "order_server/order_server.h"

namespace {
volatile std::sig_atomic_t stopping = 0;

void signalHandler(int) {
  stopping = 1;
}
}

int main(int argc, char** argv) {
  Common::ExchangeConfig config;
  try {
    const auto options = Common::startupOptions(argc, argv, "exchange.json");
    if (options.first_positional != argc)
      throw Common::ConfigError("用法：exchange_main [--config 文件] [--check-config]");
    config = Common::loadExchangeConfig(options.config_path);
    if (options.check_only) {
      std::cout << "交易所配置有效：" << options.config_path << '\n';
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "交易所启动配置错误：" << error.what() << '\n';
    return 2;
  }

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  Common::Logger logger("exchange_main.log");
  Common::CriticalJournal audit("exchange_orders.journal");
  Exchange::ClientRequestLFQueue requests(Common::QueueCapacity<Common::ME_MAX_CLIENT_UPDATES>{});
  Exchange::ClientResponseLFQueue responses(Common::QueueCapacity<Common::ME_MAX_CLIENT_UPDATES>{});
  Exchange::MEMarketUpdateLFQueue updates(Common::QueueCapacity<Common::ME_MAX_MARKET_UPDATES>{});
  auto engine = std::make_unique<Exchange::MatchingEngine>(&requests, &responses, &updates, &audit);

  // 构造参数仍是快照在前、增量在后，按字段名称传递，避免把两条流接反。
  const auto& market = config.market_data;
  auto publisher = std::make_unique<Exchange::MarketDataPublisher>(
      &updates, market.interface, market.snapshot.address, market.snapshot.port,
      market.incremental.address, market.incremental.port);
  auto server = std::make_unique<Exchange::OrderServer>(
      &requests, &responses, market.interface, config.tcp.port, &audit, config.tcp.address);

  engine->start();
  publisher->start();
  server->start();
  while (!stopping)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  server->quiesce();
  engine->stop();
  publisher->stop();
  server->stop();
  logger.log(
      "EXCHANGE STOPPED request_queue:% response_queue:% md_queue:% request_high:% response_high:% md_high:% audit_records:%\n",
      requests.size(), responses.size(), updates.size(), requests.high_watermark(),
      responses.high_watermark(), updates.high_watermark(), audit.records());
  return 0;
}
