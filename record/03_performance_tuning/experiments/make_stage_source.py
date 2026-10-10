from pathlib import Path
import zipfile,shutil
base=Path(__file__).parent
root=Path('/home/zjh/trade-measurements/03/stage_source_review')
if root.exists(): raise RuntimeError('preserve existing diagnostic source; choose a new output directory')
with zipfile.ZipFile(Path('/home/zjh/trade-measurements/03/baseline_source.zip')) as z:z.extractall(root)
shutil.copy2(Path('/home/zjh/XiangMu/TradeSystem-rebuild/common/perf_utils.h'),root/'common/perf_utils.h')
shutil.copy2(base/'stage_trace.h',root/'common/stage_trace.h')
def change(name,replacements,include=True):
    p=root/name;s=p.read_text()
    if include:s='#include "common/stage_trace.h"\n'+s
    for old,new in replacements:
        if s.count(old)!=1:raise ValueError((name,old,s.count(old)))
        s=s.replace(old,new)
    p.write_text(s,encoding='utf-8',newline='\n')
change('exchange/order_server/order_server.h',[
 ('if(audit_)audit_->request("RECEIVED",r);','StageTrace::event(StageTrace::request(r),0);\n          if(audit_)audit_->request("RECEIVED",r);\n          StageTrace::event(StageTrace::request(r),1);'),
 ('TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_);','StageTrace::event(StageTrace::response(*client_response),8,outgoing_responses_->size());\n          TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_);'),
 ('outgoing_responses_->pop();','auto* destination=cid_tcp_socket_[client_response->client_id_];\n          StageTrace::event(StageTrace::response(*client_response),9,destination->pending_bytes());\n          destination->stage_completions.emplace_back(destination->sent_bytes_+destination->pending_bytes(),StageTrace::response(*client_response));\n          outgoing_responses_->pop();')])
change('exchange/order_server/fifo_sequencer.h',[
 ('if(!incoming_requests_->try_push(client_request.request_)) {','const auto published=StageTrace::now();\n        if(!incoming_requests_->try_push(client_request.request_)) {'),
 ('TTT_MEASURE(T2_OrderServer_LFQueue_write, (*logger_));','StageTrace::at(StageTrace::request(client_request.request_),2,published,incoming_requests_->size());\n        TTT_MEASURE(T2_OrderServer_LFQueue_write, (*logger_));')])
change('exchange/matcher/matching_engine.h',[
 ('TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);','StageTrace::event(StageTrace::request(*me_client_request),11,incoming_requests_->size());\n          TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);'),
 ('processClientRequest(me_client_request);','processClientRequest(me_client_request);\n          StageTrace::event(StageTrace::request(*me_client_request),12);'),
 ('if(audit_)audit_->request("APPLY",*client_request);','StageTrace::event(StageTrace::request(*client_request),3);\n      if(audit_)audit_->request("APPLY",*client_request);\n      StageTrace::event(StageTrace::request(*client_request),4);'),
 ('if(audit_)audit_->response(response);','StageTrace::event(StageTrace::response(response),5);\n      if(audit_)audit_->response(response);\n      StageTrace::event(StageTrace::response(response),6);\n      const auto published=StageTrace::now();'),
 ('++response_count_;','StageTrace::at(StageTrace::response(response),7,published,outgoing_ogw_responses_->size());\n      ++response_count_;')])
change('common/tcp_socket.h',[
 ('#include <functional>','#include <functional>\n#include <deque>'),
 ('uint64_t sent_bytes_=0,received_bytes_=0,send_would_block_=0;','uint64_t sent_bytes_=0,received_bytes_=0,send_would_block_=0;\n  std::deque<std::pair<uint64_t,StageTrace::Key>> stage_completions;')])
change('common/tcp_socket.cpp',[
 ('const auto n=::send(socket_fd_,outbound_data_.data()+send_begin_,next_send_valid_index_,MSG_DONTWAIT|MSG_NOSIGNAL);','const auto entering=StageTrace::now();\n    const auto n=::send(socket_fd_,outbound_data_.data()+send_begin_,next_send_valid_index_,MSG_DONTWAIT|MSG_NOSIGNAL);'),
 ('if(n>0) {send_begin_+=n;next_send_valid_index_-=n;sent_bytes_+=n;}','if(n>0) {send_begin_+=n;next_send_valid_index_-=n;sent_bytes_+=n;\n      const auto accepted=StageTrace::now();\n      while(!stage_completions.empty() && stage_completions.front().first<=sent_bytes_) {StageTrace::at(stage_completions.front().second,13,entering);StageTrace::at(stage_completions.front().second,10,accepted,next_send_valid_index_);stage_completions.pop_front();}\n    }')])
change('exchange/market_data/snapshot_synthesizer.cpp',[
 ('auto SnapshotSynthesizer::publishSnapshot() {','auto SnapshotSynthesizer::publishSnapshot() {\n    StageTrace::event({0,0,snapshot_cycle_+1,0},14);'),
 ('logger_.log("SNAPSHOT cycle:% watermark:% hash:% orders:%\\n",cycle,last_inc_seq_num_,hash,live.size());','logger_.log("SNAPSHOT cycle:% watermark:% hash:% orders:%\\n",cycle,last_inc_seq_num_,hash,live.size());\n    StageTrace::event({0,0,cycle,0},15,live.size());')])
print(root)
