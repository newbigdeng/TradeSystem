#pragma once
#include <algorithm>
#include <vector>
#include <tuple>
#include "exchange/market_data/market_update.h"
namespace Common {
// Deterministic state checksum; excludes transport padding and event type.
inline uint64_t bookHash(std::vector<Exchange::MEMarketUpdate> orders) {
  std::sort(orders.begin(),orders.end(),[](const auto& a,const auto& b){return std::tie(a.ticker_id_,a.order_id_)<std::tie(b.ticker_id_,b.order_id_);});
  uint64_t hash=14695981039346656037ULL;
  auto mix=[&](uint64_t value){for(int shift=56;shift>=0;shift-=8){hash^=(value>>shift)&255;hash*=1099511628211ULL;}};
  for(const auto& o:orders) {mix(o.ticker_id_);mix(o.order_id_);mix(static_cast<uint64_t>(o.side_));mix(static_cast<uint64_t>(o.price_));mix(o.qty_);mix(o.priority_);}
  return hash;
}
}
