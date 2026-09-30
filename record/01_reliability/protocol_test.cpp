#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef TRADE_PROTOCOL_BASELINE
#include "exchange/order_server/client_request.h"
using Packet=std::array<uint8_t,sizeof(Exchange::OMClientRequest)>;
__attribute__((noinline)) Packet encode_packet(const Exchange::OMClientRequest& r) {Packet p{};memcpy(p.data(),&r,sizeof(r));return p;}
__attribute__((noinline)) bool decode_packet(const Packet& p,Exchange::OMClientRequest& r) {memcpy(&r,p.data(),sizeof(r));return true;}
#else
#include "common/order_protocol.h"
using Packet=Common::Wire::Bytes<Common::Wire::RequestSize>;
__attribute__((noinline)) Packet encode_packet(const Exchange::OMClientRequest& r) {return Common::Wire::encode(r);}
__attribute__((noinline)) bool decode_packet(const Packet& p,Exchange::OMClientRequest& r) {return Common::Wire::decode(p.data(),r);}
#endif
void require_protocol(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(int argc,char** argv) {
  try {
    const bool benchmark=argc>1 && std::string(argv[1])=="benchmark";
    Exchange::OMClientRequest input{0x0102030405060708ULL,{Exchange::ClientRequestType::NEW,2,3,4,Common::Side::SELL,1234567,9}};
#ifndef TRADE_PROTOCOL_BASELINE
    input.session_epoch_=0x1122334455667788ULL;
#endif
    if(benchmark) {
      constexpr uint64_t count=1000000;uint64_t checksum=0;const auto begin=std::chrono::steady_clock::now();
      for(uint64_t i=0;i<count;++i) {input.seq_num_=i;input.me_client_request_.order_id_=i+1;const auto bytes=encode_packet(input);Exchange::OMClientRequest output;require_protocol(decode_packet(bytes,output),"decode failed");checksum+=output.me_client_request_.order_id_;}
      require_protocol(checksum==count*(count+1)/2,"protocol checksum differs");
      const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
      std::cout<<"{\"mode\":\"protocol\",\"operations\":"<<count<<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/count<<",\"checksum\":"<<checksum<<",\"frame_bytes\":"<<sizeof(Packet)<<"}\n";return 0;
    }
    auto bytes=encode_packet(input);
    require_protocol(bytes[0]=='T'&&bytes[1]=='S'&&bytes[2]==1&&bytes[3]==1&&bytes[4]==1&&bytes[11]==8,"wire lacks a versioned big-endian header");
    Exchange::OMClientRequest output;require_protocol(decode_packet(bytes,output),"decode failed");
    require_protocol(output.me_client_request_.toString()==input.me_client_request_.toString()&&output.seq_num_==input.seq_num_,"request roundtrip differs");
#ifndef TRADE_PROTOCOL_BASELINE
    require_protocol(output.session_epoch_==input.session_epoch_,"epoch lost");bytes[2]=99;require_protocol(!decode_packet(bytes,output),"unknown protocol version accepted");
    auto bad=input.me_client_request_;bad.client_id_=Common::ME_MAX_NUM_CLIENTS;require_protocol(Common::Wire::validate(bad)==Exchange::RejectReason::INVALID_ID,"invalid ID allowed");
    bad=input.me_client_request_;bad.type_=static_cast<Exchange::ClientRequestType>(99);require_protocol(Common::Wire::validate(bad)==Exchange::RejectReason::INVALID_TYPE,"invalid type allowed");
    bad=input.me_client_request_;bad.qty_=0;require_protocol(Common::Wire::validate(bad)==Exchange::RejectReason::INVALID_QTY,"zero quantity allowed");
    Exchange::OMClientResponse response{7,{Exchange::ClientResponseType::FILLED,2,3,4,5,Common::Side::SELL,1234567,2,7,Exchange::RejectReason::NONE,11,-2},input.session_epoch_};
    const auto encoded=Common::Wire::encode(response);Exchange::OMClientResponse decoded;
    require_protocol(Common::Wire::decode(encoded.data(),decoded)&&decoded.me_client_response_.position_==-2&&decoded.me_client_response_.response_id_==11,"response roundtrip differs");
#endif
    std::cout<<"PASS protocol version, byte order, IDs, quantities, session and execution identity\n";return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
