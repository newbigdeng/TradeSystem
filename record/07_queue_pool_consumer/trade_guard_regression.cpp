#include "trade_guard_probe.h"
#include <sys/wait.h>
#include <unistd.h>

using namespace TradeGuardProbe;

void legalCase() {
  Fixture fixture;
  const auto generation = fixture.engine->marketGeneration().load();
  require(!fixture.engine->marketReady(), "unrecovered market started trusted");
  MEMarketUpdate stale;
  stale.type_ = MarketUpdateType::RECOVERY_COMMIT;
  stale.priority_ = generation - 1;
  fixture.consume(stale);
  require(!fixture.engine->marketReady() && fixture.observer->callbacks == 0,
          "old marker enabled market or invoked strategy");
  fixture.trust();
  fixture.seed();
  const auto order_free = fixture.book().order_pool_.available();
  const auto price_free = fixture.book().orders_at_price_pool_.available();
  for (uint64_t cycle = 0; cycle < 8; ++cycle) {
    fixture.consume({MarketUpdateType::ADD, BidOrder, Ticker, Side::BUY, 100, 5, 9});
    auto* object = fixture.book().oid_to_order_[BidOrder];
    require(object && object->qty_ == 5 && object->price_ == 100 && object->priority_ == 9 &&
                fixture.book().order_pool_.available() + 1 == order_free &&
                fixture.book().orders_at_price_pool_.available() + 1 == price_free,
            "ADD fields or pool consumption differ");
    require(std::abs(fixture.engine->feature_engine_.getMktPrice(Ticker) - 1250.0 / 12.0) < 1e-9,
            "ADD feature did not use current BBO");
    fixture.consume({MarketUpdateType::MODIFY, BidOrder, Ticker, Side::BUY, 100, 3, 9});
    require(fixture.book().oid_to_order_[BidOrder] == object && object->qty_ == 3 &&
                object->priority_ == 9 &&
                fixture.book().order_pool_.available() + 1 == order_free,
            "MODIFY changed object identity, quantity, priority or pool");
    require(std::abs(fixture.engine->feature_engine_.getMktPrice(Ticker) - 103.0) < 1e-9,
            "MODIFY feature did not use new quantity");
    fixture.consume({MarketUpdateType::CANCEL, BidOrder, Ticker, Side::BUY, 100, 3, 9});
    fixture.validateEnd(order_free, price_free, generation, cycle + 1);
  }
  // 合法 ticker 的非法订单状态仍使行情失效；不绕过原有簿状态检查。
  fixture.consume({MarketUpdateType::MODIFY, 31, Ticker, Side::BUY, 100, 3, 9});
  require(fixture.engine->marketGeneration().load() == generation + 1 &&
              !fixture.engine->marketReady() && fixture.observer->callbacks == 24,
          "invalid book transition did not disable market");
  stale.priority_ = generation;
  fixture.consume(stale);
  require(!fixture.engine->marketReady(), "obsolete marker re-enabled invalid market");
  fixture.trust();
  require(fixture.engine->marketReady() && fixture.observer->callbacks == 24,
          "new generation marker did not restore trust without a callback");
}

void illegalTicker(TickerId ticker) {
  // 在构造任何 Logger/TradeEngine 前 fork，子进程独立创建后台日志线程。
  int channel[2];
  require(::pipe(channel) == 0, "death-test pipe failed");
  const auto child = ::fork();
  require(child >= 0, "death-test fork failed");
  if (child == 0) {
    ::close(channel[0]);
    ::dup2(channel[1], STDERR_FILENO);
    ::close(channel[1]);
    Fixture fixture;
    fixture.consume({MarketUpdateType::ADD, BidOrder, ticker, Side::BUY, 100, 5, 9});
    _exit(0);
  }
  ::close(channel[1]);
  std::string error;
  char bytes[512];
  for (ssize_t size; (size = ::read(channel[0], bytes, sizeof(bytes))) > 0;)
    error.append(bytes, static_cast<size_t>(size));
  ::close(channel[0]);
  int status = 0;
  require(::waitpid(child, &status, 0) == child, "death-test wait failed");
  require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE &&
              error.find("ASSERT : Unknown ticker-id on update:MEMarketUpdate") != std::string::npos &&
              error.find("type:ADD") != std::string::npos,
          "illegal ticker did not fail closed with original diagnostic");
  std::cout << "PASS illegal ticker " << ticker << " failed with original diagnostic\n";
}

int main() {
  try {
    // 所有死亡测试先运行；父进程此时没有正在运行的日志线程。
    illegalTicker(ME_MAX_TICKERS);
    illegalTicker(TickerId_INVALID);
    legalCase();
    validateFooter();
    std::cout << "PASS real TradeEngine::run queue/book/ADD/MODIFY/CANCEL/BBO/pool/feature/"
                 "generation/callback; marker priority; illegal transition and ticker\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
