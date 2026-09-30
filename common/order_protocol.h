#pragma once
#include <array>
#include <bit>
#include <type_traits>
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
namespace Common::Wire {
constexpr uint8_t Version=1;
constexpr size_t RequestSize=50,ResponseSize=79;
template<size_t N> using Bytes=std::array<uint8_t,N>;
template<class T> void put(uint8_t*& output,T value) noexcept {
  using U=std::make_unsigned_t<T>;const U bits=std::bit_cast<U>(value);
  for(size_t i=sizeof(T);i>0;--i)*output++=static_cast<uint8_t>(bits>>((i-1)*8));
}
template<class T> T get(const uint8_t*& input) noexcept {
  using U=std::make_unsigned_t<T>;U bits=0;
  for(size_t i=0;i<sizeof(T);++i)bits=static_cast<U>((bits<<8)|*input++);
  return std::bit_cast<T>(bits);
}
inline void header(uint8_t*& p,uint8_t kind,uint64_t sequence,uint64_t epoch) noexcept {
  put(p,uint8_t('T'));put(p,uint8_t('S'));put(p,Version);put(p,kind);put(p,sequence);put(p,epoch);
}
inline bool header(const uint8_t*& p,uint8_t kind,uint64_t& sequence,uint64_t& epoch) noexcept {
  if(get<uint8_t>(p)!='T' || get<uint8_t>(p)!='S' || get<uint8_t>(p)!=Version || get<uint8_t>(p)!=kind)return false;
  sequence=get<uint64_t>(p);epoch=get<uint64_t>(p);return true;
}
inline Bytes<RequestSize> encode(const Exchange::OMClientRequest& message) noexcept {
  Bytes<RequestSize> bytes{};auto *p=bytes.data();header(p,1,message.seq_num_,message.session_epoch_);
  const auto &r=message.me_client_request_;
  put(p,static_cast<uint8_t>(r.type_));put(p,r.client_id_);put(p,r.ticker_id_);put(p,r.order_id_);
  put(p,static_cast<int8_t>(r.side_));put(p,r.price_);put(p,r.qty_);return bytes;
}
inline bool decode(const uint8_t* p,Exchange::OMClientRequest& message) noexcept {
  if(!header(p,1,message.seq_num_,message.session_epoch_))return false;
  auto &r=message.me_client_request_;r.type_=static_cast<Exchange::ClientRequestType>(get<uint8_t>(p));
  r.client_id_=get<uint32_t>(p);r.ticker_id_=get<uint32_t>(p);r.order_id_=get<uint64_t>(p);
  r.side_=static_cast<Side>(get<int8_t>(p));r.price_=get<int64_t>(p);r.qty_=get<uint32_t>(p);return true;
}
inline Bytes<ResponseSize> encode(const Exchange::OMClientResponse& message) noexcept {
  Bytes<ResponseSize> bytes{};auto *p=bytes.data();header(p,2,message.seq_num_,message.session_epoch_);
  const auto &r=message.me_client_response_;
  put(p,static_cast<uint8_t>(r.type_));put(p,r.client_id_);put(p,r.ticker_id_);put(p,r.client_order_id_);put(p,r.market_order_id_);
  put(p,static_cast<int8_t>(r.side_));put(p,r.price_);put(p,r.exec_qty_);put(p,r.leaves_qty_);
  put(p,static_cast<uint8_t>(r.reject_reason_));put(p,r.response_id_);put(p,r.position_);return bytes;
}
inline bool decode(const uint8_t* p,Exchange::OMClientResponse& message) noexcept {
  if(!header(p,2,message.seq_num_,message.session_epoch_))return false;
  auto &r=message.me_client_response_;r.type_=static_cast<Exchange::ClientResponseType>(get<uint8_t>(p));
  r.client_id_=get<uint32_t>(p);r.ticker_id_=get<uint32_t>(p);r.client_order_id_=get<uint64_t>(p);r.market_order_id_=get<uint64_t>(p);
  r.side_=static_cast<Side>(get<int8_t>(p));r.price_=get<int64_t>(p);r.exec_qty_=get<uint32_t>(p);r.leaves_qty_=get<uint32_t>(p);
  r.reject_reason_=static_cast<Exchange::RejectReason>(get<uint8_t>(p));r.response_id_=get<uint64_t>(p);r.position_=get<int64_t>(p);return true;
}
inline Exchange::RejectReason validate(const Exchange::MEClientRequest& r) noexcept {
  using Exchange::RejectReason;using Exchange::ClientRequestType;
  if(r.client_id_>=ME_MAX_NUM_CLIENTS || r.ticker_id_>=ME_MAX_TICKERS || r.order_id_>=ME_MAX_ORDER_IDS)return RejectReason::INVALID_ID;
  if(r.type_==ClientRequestType::QUERY)return RejectReason::NONE; // ID zero queries positions
  if(r.type_!=ClientRequestType::NEW && r.type_!=ClientRequestType::CANCEL)return RejectReason::INVALID_TYPE;
  if(!r.order_id_)return RejectReason::INVALID_ID;
  if(r.type_==ClientRequestType::CANCEL)return RejectReason::NONE;
  if(r.side_!=Side::BUY && r.side_!=Side::SELL)return RejectReason::INVALID_SIDE;
  if(r.price_<=0 || r.price_==Price_INVALID)return RejectReason::INVALID_PRICE;
  if(!r.qty_ || r.qty_==Qty_INVALID || r.qty_>INT32_MAX)return RejectReason::INVALID_QTY;
  return RejectReason::NONE;
}
}
