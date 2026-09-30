#include <csignal>
#include <cmath>
#include <filesystem>
#include <memory>
#include "strategy/trade_engine.h"
#include "order_gw/order_gateway.h"
#include "market_data/market_data_consumer.h"
#include "common/critical_journal.h"
namespace {volatile std::sig_atomic_t stopping=0;void signalHandler(int){stopping=1;}
uint64_t setting(const char* key,uint64_t fallback,uint64_t maximum) {
  const auto* text=std::getenv(key);if(!text)return fallback;
  const auto value=std::stoull(text);ASSERT(value<=maximum,"environment setting out of bounds: "+std::string(key));return value;
}}
int main(int argc,char** argv) {
  if(argc<3 || (argc-3)%5 || (argc-3)/5>int(Common::ME_MAX_TICKERS)) {std::cerr<<"USAGE trading_main CLIENT_ID ALGO [CLIP THRESH MAX_ORDER_SIZE MAX_POSITION MAX_LOSS] ...\n";return 2;}
  const auto id=std::stoul(argv[1]);ASSERT(id>0&&id<Common::ME_MAX_NUM_CLIENTS,"invalid client ID");
  const auto client=Common::ClientId(id);const auto algo=Common::stringToAlgoType(argv[2]);ASSERT(algo!=Common::AlgoType::INVALID&&algo!=Common::AlgoType::MAX,"invalid algorithm");
  const std::string checkpoint="trading_account_"+std::to_string(client)+".journal";
  ASSERT(!std::filesystem::exists(checkpoint),"previous trading checkpoint requires reconciliation or an explicit simulation reset: "+checkpoint);
  Common::TradeEngineCfgHashMap config{};
  for(auto& cfg:config)cfg={10,1,{100,1000,-1000000}};
  for(int i=3;i<argc;i+=5) {
    auto& cfg=config.at((i-3)/5);
    const auto clip=std::stoull(argv[i]),max_order=std::stoull(argv[i+2]),max_position=std::stoull(argv[i+3]);
    const auto threshold=std::stod(argv[i+1]),loss=std::stod(argv[i+4]);
    ASSERT(clip>0&&clip<=max_order&&max_order<=INT32_MAX&&max_position<=INT32_MAX&&std::isfinite(threshold)&&threshold>=0&&std::isfinite(loss)&&loss<=0,"invalid strategy/risk configuration");
    cfg={Common::Qty(clip),threshold,{Common::Qty(max_order),Common::Qty(max_position),loss}};
  }
  std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);std::srand(client);
  Common::Logger logger("trading_main_"+std::to_string(client)+".log");
  Exchange::ClientRequestLFQueue requests(Common::ME_MAX_CLIENT_UPDATES);Exchange::ClientResponseLFQueue responses(Common::ME_MAX_CLIENT_UPDATES);Exchange::MEMarketUpdateLFQueue updates(Common::ME_MAX_MARKET_UPDATES);
  auto engine=std::make_unique<Trading::TradeEngine>(client,algo,config,&requests,&responses,&updates);
  auto gateway=std::make_unique<Trading::OrderGateway>(client,&requests,&responses,"127.0.0.1","lo",12345);
  engine->setOrderSession(&gateway->sessionHealthy());
  auto consumer=std::make_unique<Trading::MarketDataConsumer>(client,&updates,"lo","233.252.14.1",20000,"233.252.14.3",20001,&engine->marketTrust(),&engine->marketGeneration());
  engine->start();gateway->start();consumer->start();
  const auto seconds=setting("TRADE_RUN_SECONDS",0,86400);
  const auto deadline=seconds?std::chrono::steady_clock::now()+std::chrono::seconds(seconds):std::chrono::steady_clock::time_point::max();
  auto active=[&]{return !stopping&&std::chrono::steady_clock::now()<deadline&&gateway->sessionHealthy().load();};
  const auto initial_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(75);
  while(active()&&!engine->marketTrust().load()&&std::chrono::steady_clock::now()<initial_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if(algo==Common::AlgoType::RANDOM) {
    Common::OrderId next_order=client*1000;std::vector<Exchange::MEClientRequest> sent;
    std::array<Common::Price,Common::ME_MAX_TICKERS> base{};for(auto& price:base)price=100+std::rand()%100;
    const auto count=setting("TRADE_RANDOM_ORDERS",10000,900000),delay=setting("TRADE_RANDOM_DELAY_US",20000,1000000);
    for(uint64_t i=0;i<count&&active();++i) {
      const Common::TickerId ticker=std::rand()%Common::ME_MAX_TICKERS;
      const Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,client,ticker,next_order++,std::rand()%2?Common::Side::BUY:Common::Side::SELL,base[ticker]+1+std::rand()%10,Common::Qty(2+std::rand()%100)};
      if(engine->sendClientRequest(&request))sent.push_back(request);
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
      if(!sent.empty()) {auto cancel=sent[std::rand()%sent.size()];cancel.type_=Exchange::ClientRequestType::CANCEL;engine->sendClientRequest(&cancel);}
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
    if(!seconds)stopping=1;
  }
  while(active())std::this_thread::sleep_for(std::chrono::milliseconds(10));
  engine->quiesce();consumer->stop();gateway->stop();engine->stop();
  Common::CriticalJournal account(checkpoint);engine->writeCheckpoint(account,gateway->sessionEpoch(),gateway->sessionHealthy().load());
  logger.log("TRADING STOPPED request_queue:% response_queue:% md_queue:% session_healthy:%\n",requests.size(),responses.size(),updates.size(),gateway->sessionHealthy().load());
  return gateway->sessionHealthy().load()?0:3;
}
