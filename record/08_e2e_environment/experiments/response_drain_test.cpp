#include <cstring>
#include <memory>
#include <stdexcept>
#include "exchange/order_server/order_server.h"

void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
  Common::Logger logger("response_drain_test.log");
  Exchange::ClientRequestLFQueue requests(8);
  Exchange::ClientResponseLFQueue responses(8);
  auto server=std::make_unique<Exchange::OrderServer>(&requests,&responses,"lo",0);
  Common::TCPSocket socket(logger,2*Common::Wire::ResponseSize);
  auto receive=[&](uint64_t seq) {
    Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,1,0,seq,Common::Side::BUY,100,1};
    const auto frame=Common::Wire::encode(Exchange::OMClientRequest{seq,request,77});
    std::memcpy(socket.inbound_data_.data(),frame.data(),frame.size());
    socket.next_rcv_valid_index_=frame.size();
    server->recvCallback(&socket,seq);
    check(socket.next_rcv_valid_index_==0,"request was not consumed");
  };
  auto respond=[&](uint64_t id) {
    Exchange::MEClientResponse response{};
    response.type_=Exchange::ClientResponseType::ACCEPTED;
    response.client_id_=1;response.ticker_id_=0;response.client_order_id_=id;
    response.response_id_=100+id;
    check(responses.try_push(response),"response queue full");
  };
  auto verify=[&](size_t offset,uint64_t seq) {
    Exchange::OMClientResponse response;
    check(Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket.outbound_data_.data()+offset),response),"decode response failed");
    check(response.seq_num_==seq && response.session_epoch_==77 && response.me_client_response_.response_id_==100+seq,"response sequence/identity changed");
  };
  receive(1);respond(1);receive(2);
  check(responses.peek()==nullptr && socket.pending_bytes()==Common::Wire::ResponseSize,"ready response waited until receive callback finished");verify(0,1);
  respond(2);receive(3);verify(Common::Wire::ResponseSize,2);
  respond(3);receive(4);
  check(responses.peek()!=nullptr && responses.peek()->response_id_==103,"full buffer consumed response prematurely");
  socket.next_send_valid_index_=0; // Simulate an already tested socket flush; this test exercises callback scheduling.
  receive(5);check(responses.peek()==nullptr,"response did not resume after buffer space returned");verify(0,3);
  server->recvFinishedCallback();
  for(uint64_t seq=1;seq<=5;++seq) {
    Exchange::MEClientRequest request;
    check(requests.try_pop(request) && request.request_seq_==seq && request.session_epoch_==77,"request FIFO or session changed");
  }
  check(requests.peek()==nullptr,"unexpected request");
}
