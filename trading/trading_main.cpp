// 装配交易引擎、TCP 订单网关和两路组播接收器；当前示例端点使用 lo 和本机地址。
// 先等待行情恢复，再按策略运行；订单会话失效时停止发送并保存对账状态。
// 停机先禁止新请求，再停行情、排空网关、停交易引擎，最后写账户检查点。

#include <csignal> // 引入退出信号。
#include <cmath> // 引入浮点数有限性检查。
#include <filesystem> // 引入检查点文件检查。
#include <memory> // 引入资源生命周期管理。
#include "strategy/trade_engine.h" // 引入strategy/trade_engine.h 的声明。
#include "order_gw/order_gateway.h" // 引入order_gw/order_gateway.h 的声明。
#include "market_data/market_data_consumer.h" // 引入market_data/market_data_consumer.h 的声明。
#include "common/critical_journal.h" // 引入common/critical_journal.h 的声明。
namespace {volatile std::sig_atomic_t stopping=0;void signalHandler(int){stopping=1;} // 信号处理器只设置退出标志，不执行日志和复杂清理。
uint64_t setting(const char* key,uint64_t fallback,uint64_t maximum) { // 读取有上限约束的环境配置。
  const auto* text=std::getenv(key);if(!text)return fallback; // 未配置时返回默认值。
  const auto value=std::stoull(text);ASSERT(value<=maximum,"environment setting out of bounds: "+std::string(key));return value; // 解析无符号整数并校验上限。
}} // 结束当前作用域。
int main(int argc,char** argv) { // 组装并运行交易客户端。
  if(argc<3 || (argc-3)%5 || (argc-3)/5>int(Common::ME_MAX_TICKERS)) {std::cerr<<"USAGE trading_main CLIENT_ID ALGO [CLIP THRESH MAX_ORDER_SIZE MAX_POSITION MAX_LOSS] ...\n";return 2;} // 检查客户、算法和每品种五项配置的参数数量。
  const auto id=std::stoul(argv[1]);ASSERT(id>0&&id<Common::ME_MAX_NUM_CLIENTS,"invalid client ID"); // 客户 ID 必须处于允许范围。
  const auto client=Common::ClientId(id);const auto algo=Common::stringToAlgoType(argv[2]);ASSERT(algo!=Common::AlgoType::INVALID&&algo!=Common::AlgoType::MAX,"invalid algorithm"); // 转换并校验算法类型。
  const std::string checkpoint="trading_account_"+std::to_string(client)+".journal"; // 生成该客户的账户检查点文件名。
  ASSERT(!std::filesystem::exists(checkpoint),"previous trading checkpoint requires reconciliation or an explicit simulation reset: "+checkpoint); // 已有检查点时拒绝无对账重启。
  Common::TradeEngineCfgHashMap config{}; // 初始化各品种配置。
  for(auto& cfg:config)cfg={10,1,{100,1000,-1000000}}; // 设置默认下单量、阈值和风险限额。
  for(int i=3;i<argc;i+=5) { // 每五个命令行参数描述一个品种。
    auto& cfg=config.at((i-3)/5); // 定位该品种配置。
    const auto clip=std::stoull(argv[i]),max_order=std::stoull(argv[i+2]),max_position=std::stoull(argv[i+3]); // 读取下单量、单笔上限和持仓上限。
    const auto threshold=std::stod(argv[i+1]),loss=std::stod(argv[i+4]); // 读取策略阈值和最大亏损。
    ASSERT(clip>0&&clip<=max_order&&max_order<=INT32_MAX&&max_position<=INT32_MAX&&std::isfinite(threshold)&&threshold>=0&&std::isfinite(loss)&&loss<=0,"invalid strategy/risk configuration"); // 校验范围、大小关系和浮点数有限性。
    cfg={Common::Qty(clip),threshold,{Common::Qty(max_order),Common::Qty(max_position),loss}}; // 转换为业务类型并保存配置。
  } // 结束当前作用域。

  std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);std::srand(client); // 注册退出信号，按客户 ID 固定随机种子。
  Common::Logger logger("trading_main_"+std::to_string(client)+".log"); // 创建主线程日志。
  Exchange::ClientRequestLFQueue requests(Common::ME_MAX_CLIENT_UPDATES);Exchange::ClientResponseLFQueue responses(Common::ME_MAX_CLIENT_UPDATES);Exchange::MEMarketUpdateLFQueue updates(Common::ME_MAX_MARKET_UPDATES); // 构造三条固定容量队列；大缓冲仍在堆上。
  auto engine=std::make_unique<Trading::TradeEngine>(client,algo,config,&requests,&responses,&updates); // 构造交易引擎并连接三条队列。
  auto gateway=std::make_unique<Trading::OrderGateway>(client,&requests,&responses,"127.0.0.1","lo",12345); // 构造连接本机 12345 端口的订单网关。
  engine->setOrderSession(&gateway->sessionHealthy()); // 把订单会话健康状态接入发送门禁。
  auto consumer=std::make_unique<Trading::MarketDataConsumer>(client,&updates,"lo","233.252.14.1",20000,"233.252.14.3",20001,&engine->marketTrust(),&engine->marketGeneration()); // 在 lo 订阅两路组播，并共享行情信任与代次。
  engine->start();gateway->start();consumer->start(); // 先启动消费端，再启动订单和行情网络线程。
  const auto seconds=setting("TRADE_RUN_SECONDS",0,86400); // 读取运行时长，0 表示不设时间截止。
  const auto deadline=seconds?std::chrono::steady_clock::now()+std::chrono::seconds(seconds):std::chrono::steady_clock::time_point::max(); // 用单调时钟计算运行期限。
  auto active=[&]{return !stopping&&std::chrono::steady_clock::now()<deadline&&gateway->sessionHealthy().load();}; // 退出信号、时间期限或会话失效都会结束运行。
  const auto initial_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(75); // 首次等待完整行情恢复最多 75 秒。
  while(active()&&!engine->marketTrust().load()&&std::chrono::steady_clock::now()<initial_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 恢复前暂停策略启动，周期性检查信任状态。
  if(algo==Common::AlgoType::RANDOM) { // RANDOM 模式由主线程生成模拟请求。
    Common::OrderId next_order=client*1000;std::vector<Exchange::MEClientRequest> sent; // 准备订单 ID 和已成功发送请求集合。
    std::array<Common::Price,Common::ME_MAX_TICKERS> base{};for(auto& price:base)price=100+std::rand()%100; // 为每个品种生成固定基准价。
    const auto count=setting("TRADE_RANDOM_ORDERS",10000,900000),delay=setting("TRADE_RANDOM_DELAY_US",20000,1000000); // 读取模拟请求数量和间隔。
    for(uint64_t i=0;i<count&&active();++i) { // 按数量与运行条件生成请求。
      const Common::TickerId ticker=std::rand()%Common::ME_MAX_TICKERS; // 随机选取品种。
      const Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,client,ticker,next_order++,std::rand()%2?Common::Side::BUY:Common::Side::SELL,base[ticker]+1+std::rand()%10,Common::Qty(2+std::rand()%100)}; // 构造带方向、价格和数量的新单。
      if(engine->sendClientRequest(&request))sent.push_back(request); // 通过交易引擎风控和发送门禁后才记入已发送集合。
      std::this_thread::sleep_for(std::chrono::microseconds(delay)); // 控制请求节奏。
      if(!sent.empty()) {auto cancel=sent[std::rand()%sent.size()];cancel.type_=Exchange::ClientRequestType::CANCEL;engine->sendClientRequest(&cancel);} // 随机选择已发送订单尝试整单撤销，已成交订单可能拒撤。
      std::this_thread::sleep_for(std::chrono::microseconds(delay)); // 撤单后再次等待，控制节奏。
    } // 结束当前作用域。

    if(!seconds)stopping=1; // 未配置运行期限时，生成完成即请求结束。
  } // 结束当前作用域。

  while(active())std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 其余运行时间周期性等待退出条件。
  engine->quiesce();consumer->stop();gateway->stop();engine->stop(); // 先禁止新请求，再停行情、排空网关、停止交易引擎。
  Common::CriticalJournal account(checkpoint);engine->writeCheckpoint(account,gateway->sessionEpoch(),gateway->sessionHealthy().load()); // 写入账户检查点及会话是否仍可确认。
  logger.log("TRADING STOPPED request_queue:% response_queue:% md_queue:% session_healthy:%\n",requests.size(),responses.size(),updates.size(),gateway->sessionHealthy().load()); // 记录诊断日志。
  return gateway->sessionHealthy().load()?0:3; // 会话正常返回 0，不确定状态返回 3。
} // 结束当前作用域。
