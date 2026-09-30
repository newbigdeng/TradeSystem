#include <csignal>
#include <memory>
#include "matcher/matching_engine.h"
#include "market_data/market_data_publisher.h"
#include "order_server/order_server.h"
namespace {volatile std::sig_atomic_t stopping=0;void signalHandler(int){stopping=1;}}
int main() {
  std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);
  Common::Logger logger("exchange_main.log");
  Common::CriticalJournal audit("exchange_orders.journal");
  Exchange::ClientRequestLFQueue requests(Common::ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue responses(Common::ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue updates(Common::ME_MAX_MARKET_UPDATES);
  auto engine=std::make_unique<Exchange::MatchingEngine>(&requests,&responses,&updates,&audit);
  auto publisher=std::make_unique<Exchange::MarketDataPublisher>(&updates,"lo","233.252.14.1",20000,"233.252.14.3",20001);
  auto server=std::make_unique<Exchange::OrderServer>(&requests,&responses,"lo",12345,&audit);
  engine->start();publisher->start();server->start();
  while(!stopping)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  server->quiesce();engine->stop();publisher->stop();server->stop();
  logger.log("EXCHANGE STOPPED request_queue:% response_queue:% md_queue:% request_high:% response_high:% md_high:% audit_records:%\n",requests.size(),responses.size(),updates.size(),requests.high_watermark(),responses.high_watermark(),updates.high_watermark(),audit.records());
  return 0;
}
