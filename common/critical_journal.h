#pragma once
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include "macros.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
namespace Common {
// Durable audit and restart fence, distinct from the lossy diagnostic logger.
// A previous journal requires reconciliation; it is never silently truncated.
class CriticalJournal {
 public:
  explicit CriticalJournal(const std::string& path) {
    fd_=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    ASSERT(fd_>=0,"existing/unavailable order journal: reconcile or explicitly archive the previous simulation before restarting: "+path);
    append("TS_AUDIT_V1");
    const auto slash=path.find_last_of('/');const auto directory=slash==std::string::npos?std::string("."):path.substr(0,slash);
    const int parent=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ASSERT(parent>=0,"cannot open journal parent directory");
    ASSERT(::fsync(parent)==0,"cannot persist journal directory entry");::close(parent);
  }
  ~CriticalJournal(){if(fd_>=0)::close(fd_);}
  void append(const std::string& text) {
    std::lock_guard<std::mutex> lock(writer_);const auto record=text+'\n';size_t done=0;
    while(done<record.size()) {
      const auto n=::write(fd_,record.data()+done,record.size()-done);
      if(n>0)done+=n;else if(n<0&&errno==EINTR)continue;else FATAL("critical journal write failed");
    }
    int rc;do {rc=::fsync(fd_);}while(rc<0&&errno==EINTR);
    ASSERT(rc==0,"critical journal fsync failed");++records_;
  }
  void request(const char* phase,const Exchange::MEClientRequest& r) {
    std::ostringstream text;text<<phase<<' '<<r.client_id_<<' '<<r.session_epoch_<<' '<<r.request_seq_<<' '<<int(r.type_)<<' '<<r.ticker_id_<<' '<<r.order_id_<<' '<<int(r.side_)<<' '<<r.price_<<' '<<r.qty_;append(text.str());
  }
  void response(const Exchange::MEClientResponse& r) {
    std::ostringstream text;text<<"RESPONSE "<<r.response_id_<<' '<<r.client_id_<<' '<<r.ticker_id_<<' '<<r.client_order_id_<<' '<<r.market_order_id_<<' '<<int(r.type_)<<' '<<int(r.side_)<<' '<<r.price_<<' '<<r.exec_qty_<<' '<<r.leaves_qty_<<' '<<int(r.reject_reason_)<<' '<<r.position_;append(text.str());
  }
  uint64_t records() const {return records_;} // after workers have joined
  CriticalJournal(const CriticalJournal&)=delete;CriticalJournal& operator=(const CriticalJournal&)=delete;
 private:
  int fd_=-1;std::mutex writer_;uint64_t records_=0;
};
}
