#pragma once
#include "order_protocol.h"
#include "exchange/market_data/market_update.h"
namespace Common::Wire {
constexpr size_t MarketSize=70;
inline Bytes<MarketSize> encode(const Exchange::MDPMarketUpdate& message) noexcept {
  Bytes<MarketSize> bytes{};auto *p=bytes.data();
  put(p,uint8_t('T'));put(p,uint8_t('S'));put(p,Version);put(p,uint8_t(3));
  put(p,message.seq_num_);put(p,message.snapshot_cycle_);put(p,message.watermark_);put(p,message.state_hash_);
  const auto& r=message.me_market_update_;put(p,static_cast<uint8_t>(r.type_));put(p,r.order_id_);put(p,r.ticker_id_);
  put(p,static_cast<int8_t>(r.side_));put(p,r.price_);put(p,r.qty_);put(p,r.priority_);return bytes;
}
inline bool decode(const uint8_t* p,Exchange::MDPMarketUpdate& message) noexcept {
  if(get<uint8_t>(p)!='T'||get<uint8_t>(p)!='S'||get<uint8_t>(p)!=Version||get<uint8_t>(p)!=3)return false;
  message.seq_num_=get<uint64_t>(p);message.snapshot_cycle_=get<uint64_t>(p);message.watermark_=get<uint64_t>(p);message.state_hash_=get<uint64_t>(p);
  auto& r=message.me_market_update_;r.type_=static_cast<Exchange::MarketUpdateType>(get<uint8_t>(p));r.order_id_=get<uint64_t>(p);r.ticker_id_=get<uint32_t>(p);
  r.side_=static_cast<Side>(get<int8_t>(p));r.price_=get<int64_t>(p);r.qty_=get<uint32_t>(p);r.priority_=get<uint64_t>(p);return true;
}
}
