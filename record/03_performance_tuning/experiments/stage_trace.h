#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <time.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace StageTrace {
struct Key {uint32_t client,ticker;uint64_t order;uint8_t kind;};
struct Event {uint64_t time;Key key;uint32_t stage;int cpu;uint64_t depth;};
inline uint64_t now() {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
struct Ring {
  std::array<Event,131072> events;size_t size=0;uint64_t overflow=0;
  long tid=syscall(SYS_gettid);
  ~Ring() {
    std::ofstream out("stage_"+std::to_string(tid)+".csv");
    out<<"client,ticker,order,kind,stage,time_ns,cpu,tid,queue_or_pending_bytes\n";
    for(size_t i=0;i<size;++i) {const auto& e=events[i];out<<e.key.client<<','<<e.key.ticker<<','<<e.key.order<<','<<int(e.key.kind)<<','<<e.stage<<','<<e.time<<','<<e.cpu<<','<<tid<<','<<e.depth<<'\n';}
    std::ofstream meta("stage_"+std::to_string(tid)+".json");meta<<"{\"events\":"<<size<<",\"overflow\":"<<overflow<<",\"capacity\":131072}\n";
  }
};
inline thread_local std::unique_ptr<Ring> ring;
inline void at(const Key& key,uint32_t stage,uint64_t time,uint64_t depth=0) {
  if(!ring)ring=std::make_unique<Ring>();
  if(ring->size<ring->events.size())ring->events[ring->size++]={time,key,stage,sched_getcpu(),depth};else ++ring->overflow;
}
inline void event(const Key& key,uint32_t stage,uint64_t depth=0) {const auto t=now();at(key,stage,t,depth);}
template<class R> Key request(const R& r) {return {r.client_id_,r.ticker_id_,r.order_id_,static_cast<uint8_t>(r.type_)};}
template<class R> Key response(const R& r) {return {r.client_id_,r.ticker_id_,r.client_order_id_,static_cast<uint8_t>(r.type_)};}
}
