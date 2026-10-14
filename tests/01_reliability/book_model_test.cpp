#include <deque>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <tuple>
#include "matcher/me_order_book.h"
#include "order_server/client_request.h"
#include "matcher/unordered_map_me_order_book.h"
#include "strategy/market_order_book.h"
using namespace Common;
using namespace Exchange;

void verify(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct RefOrder {
  ClientId client;
  OrderId id, market;
  Side side;
  Price price;
  Qty qty;
  Priority priority;
};

using Response = std::tuple<ClientResponseType, ClientId, OrderId, OrderId, Price, Qty, Qty>;

Response normalize(const MEClientResponse& r) {
  return {r.type_,
          r.client_id_,
          r.client_order_id_,
          r.market_order_id_,
          r.type_ == ClientResponseType::FILLED ? r.price_ : 0,
          r.type_ == ClientResponseType::FILLED ? r.exec_qty_ : 0,
          r.leaves_qty_};
}

struct Reference {
  std::map<Price, std::deque<OrderId>> bids, asks;
  std::map<OrderId, RefOrder> orders;
  OrderId next_market = 1;

  std::vector<Response> apply(const MEClientRequest& r) {
    std::vector<Response> out;
    if (r.type_ == ClientRequestType::CANCEL) {
      auto found = orders.find(r.order_id_);
      if (found == orders.end() || found->second.client != r.client_id_) {
        out.push_back({ClientResponseType::CANCEL_REJECTED, r.client_id_, r.order_id_,
                       OrderId_INVALID, 0, 0, 0});
        return out;
      }
      const auto o = found->second;
      auto& levels = o.side == Side::BUY ? bids : asks;
      auto& level = levels.at(o.price);
      level.erase(std::find(level.begin(), level.end(), o.id));
      if (level.empty())
        levels.erase(o.price);
      orders.erase(found);
      out.push_back({ClientResponseType::CANCELED, o.client, o.id, o.market, 0, 0, o.qty});
      return out;
    }
    const auto market = next_market++;
    Qty leaves = r.qty_;
    out.push_back({ClientResponseType::ACCEPTED, r.client_id_, r.order_id_, market, 0, 0, r.qty_});
    auto& opposite = r.side_ == Side::BUY ? asks : bids;
    while (leaves && !opposite.empty()) {
      const auto price = r.side_ == Side::BUY ? opposite.begin()->first : opposite.rbegin()->first;
      if ((r.side_ == Side::BUY && price > r.price_) || (r.side_ == Side::SELL && price < r.price_))
        break;
      auto& level = opposite.at(price);
      auto& passive = orders.at(level.front());
      const auto executed = std::min(leaves, passive.qty);
      leaves -= executed;
      passive.qty -= executed;
      out.push_back(
          {ClientResponseType::FILLED, r.client_id_, r.order_id_, market, price, executed, leaves});
      out.push_back({ClientResponseType::FILLED, passive.client, passive.id, passive.market, price,
                     executed, passive.qty});
      if (!passive.qty) {
        orders.erase(level.front());
        level.pop_front();
        if (level.empty())
          opposite.erase(price);
      }
    }
    if (leaves) {
      auto& level = (r.side_ == Side::BUY ? bids : asks)[r.price_];
      const Priority priority = level.empty() ? 1 : orders.at(level.back()).priority + 1;
      orders.emplace(r.order_id_, RefOrder{r.client_id_, r.order_id_, market, r.side_, r.price_,
                                           leaves, priority});
      level.push_back(r.order_id_);
    }
    return out;
  }

  auto state() const {
    std::map<OrderId, std::tuple<Side, Price, Qty, Priority>> result;
    for (const auto& [id, o] : orders) {
      (void)id;
      result[o.market] = {o.side, o.price, o.qty, o.priority};
    }
    return result;
  }
};

auto state(const std::vector<MEMarketUpdate>& orders) {
  std::map<OrderId, std::tuple<Side, Price, Qty, Priority>> result;
  for (const auto& o : orders)
    result[o.order_id_] = {o.side_, o.price_, o.qty_, o.priority_};
  return result;
}

template <class Book>
void model() {
  Common::Logger logger("book_model.log");
  Book book(0, &logger, nullptr);
  auto client = std::make_unique<Trading::MarketOrderBook>(0, &logger);
  std::vector<Response> actual;
  Reference reference;
  book.response_sink_ = [&](const auto& r) { actual.push_back(normalize(r)); };
  book.market_sink_ = [&](const auto& u) {
    verify(client->onMarketUpdate(&u), "client rejected valid market update");
  };
  std::mt19937 rng(0x51A7);
  OrderId next = 1;
  for (size_t event = 0; event < 12000; ++event) {
    MEClientRequest r;
    if (event % 4 == 0 && next > 1)
      r = {ClientRequestType::CANCEL,
           ClientId(1 + rng() % 3),
           0,
           OrderId(1 + rng() % (next - 1)),
           Side::INVALID,
           Price_INVALID,
           0};
    else
      r = {ClientRequestType::NEW,
           ClientId(1 + rng() % 3),
           0,
           next++,
           rng() % 2 ? Side::BUY : Side::SELL,
           Price(100 + 256 * (rng() % 9)),
           Qty(1 + rng() % 7)};
    actual.clear();
    const auto expected = reference.apply(r);
    if (r.type_ == ClientRequestType::NEW)
      book.add(r.client_id_, r.order_id_, 0, r.side_, r.price_, r.qty_);
    else
      book.cancel(r.client_id_, r.order_id_, 0);
    verify(actual == expected, "responses differ from price/FIFO reference model");
    const auto expected_state = reference.state();
    verify(state(book.liveOrders()) == expected_state, "central book differs from reference");
    verify(state(client->liveOrders()) == expected_state,
           "client book differs at same event watermark");
    const auto* bbo = client->getBBO();
    verify(bbo->bid_price_ ==
               (reference.bids.empty() ? Price_INVALID : reference.bids.rbegin()->first),
           "best bid differs");
    verify(
        bbo->ask_price_ == (reference.asks.empty() ? Price_INVALID : reference.asks.begin()->first),
        "best ask differs");
    auto best_quantity = [&](const auto& levels, Price best_price) {
      if (levels.empty())
        return Trading::BookQty_INVALID;
      Trading::BookQty quantity = 0;
      for (auto id : levels.at(best_price))
        quantity += reference.orders.at(id).qty;
      return quantity;
    };
    verify(bbo->bid_qty_ == best_quantity(reference.bids, bbo->bid_price_),
           "best bid quantity differs");
    verify(bbo->ask_qty_ == best_quantity(reference.asks, bbo->ask_price_),
           "best ask quantity differs");
  }
  const auto before = state(book.liveOrders());
  actual.clear();
  book.add(1, 0, 0, Side::BUY, 100, 1);
  book.add(1, next, 0, Side::BUY, -1, 1);
  book.add(1, next, 0, Side::BUY, 100, 0);
  book.add(1, next, 0, Side::MAX, 100, 1);
  book.add(ME_MAX_NUM_CLIENTS, next, 0, Side::BUY, 100, 1);
  verify(actual.size() == 5 && std::all_of(actual.begin(), actual.end(),
                                           [](const auto& r) {
                                             return std::get<0>(r) == ClientResponseType::REJECTED;
                                           }),
         "invalid orders not explicitly rejected");
  verify(state(book.liveOrders()) == before, "invalid request mutated book");
  MEMarketUpdate missing{MarketUpdateType::CANCEL, ME_MAX_ORDER_IDS, 0, Side::BUY, 100, 1, 1};
  verify(!client->onMarketUpdate(&missing), "out-of-range market ID accepted");
  MEMarketUpdate clear{MarketUpdateType::CLEAR, OrderId_INVALID, 0,
                       Side::INVALID,           Price_INVALID,   Qty_INVALID,
                       Priority_INVALID};
  verify(client->onMarketUpdate(&clear) && client->liveOrders().empty() &&
             client->getBBO()->bid_price_ == Price_INVALID,
         "CLEAR left dangling state");
}

// 接近价格档容量时，分别覆盖新最优、新最差和中间插入，再反向撤档。
template <class Book>
void level_boundaries() {
  Common::Logger logger("book_levels.log");
  Book book(0, &logger, nullptr);
  auto client = std::make_unique<Trading::MarketOrderBook>(0, &logger);
  book.response_sink_ = [](const auto&) {};
  book.market_sink_ = [&](const auto& update) {
    verify(client->onMarketUpdate(&update), "client rejected boundary event");
  };
  std::vector<size_t> ranks(128);
  std::iota(ranks.begin(), ranks.end(), 0);
  std::mt19937 rng(531);
  OrderId next = 1;
  for (int pattern = 0; pattern < 3; ++pattern) {
    if (pattern == 1)
      std::reverse(ranks.begin(), ranks.end());
    if (pattern == 2)
      std::shuffle(ranks.begin(), ranks.end(), rng);
    std::vector<OrderId> ids;
    for (auto rank : ranks) {
      book.add(1, next, 0, Side::BUY, 1000 - rank, 1);
      ids.push_back(next++);
      book.add(1, next, 0, Side::SELL, 5000 + rank, 1);
      ids.push_back(next++);
      book.toString(false, true);
      client->toString(false, true);
    }
    verify(book.liveOrders().size() == 256, "price level insertion lost orders");
    std::reverse(ids.begin(), ids.end());
    for (auto id : ids) {
      book.cancel(1, id, 0);
      book.toString(false, true);
      client->toString(false, true);
    }
    verify(book.liveOrders().empty(), "price level removal retained orders");
    verify(client->liveOrders().empty(), "client boundary removal retained orders");
  }
}

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string(argv[1]) == "unordered") {
      level_boundaries<UnorderedMapMEOrderBook>();
      model<UnorderedMapMEOrderBook>();
    } else {
      level_boundaries<MEOrderBook>();
      model<MEOrderBook>();
    }
    std::cout
        << "PASS 12000 seeded events: response FIFO, quantity, live orders, BBO, client state, invalid input\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
