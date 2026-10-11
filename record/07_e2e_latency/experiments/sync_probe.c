#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long ns(clockid_t clock) {
  struct timespec t;clock_gettime(clock,&t);return (long long)t.tv_sec*1000000000LL+t.tv_nsec;
}
int fsync(int fd) {
  const char *mode=getenv("TRADE_SYNC_MODE"),*path=getenv("TRADE_SYNC_TRACE");
  struct stat st;int data_only=mode && !strcmp(mode,"fdatasync") && fstat(fd,&st)==0 && S_ISREG(st.st_mode);
  const long long begin=ns(CLOCK_MONOTONIC),wall=ns(CLOCK_REALTIME);
  int rc=syscall(data_only?SYS_fdatasync:SYS_fsync,fd),saved=errno;
  const long long elapsed=ns(CLOCK_MONOTONIC)-begin;
  if(path) {
    int out=open(path,O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC,0600);
    if(out>=0) {
      char line[200];int len=snprintf(line,sizeof(line),"%lld,%ld,%d,%s,%lld,%d\n",wall,syscall(SYS_gettid),fd,data_only?"fdatasync":"fsync",elapsed,rc);
      if(len>0 && len<(int)sizeof(line)) { ssize_t ignored=write(out,line,len);(void)ignored; }
      close(out);
    }
  }
  errno=saved;return rc;
}
