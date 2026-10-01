#include "common/logging.h"
#include "common/opt_logging.h"
#include "record/02_reproducible_measurement/benchmark_support.h"
#include <iterator>

template<class Logger> void benchmark(const std::string& name,const std::vector<std::string>& input) {
  std::vector<uint64_t> samples(input.size());uint64_t dropped=0;
  std::string expected;for(const auto& line:input)expected+=line+'\n';
  // Warm the formatting, queue, consumer and file paths using a separate file.
  {Logger warmup(name+"_warmup.log");for(size_t i=0;i<100;++i)warmup.log("%\n",input[i]);}
  const auto window=Measure::now();
  {
    Logger logger(name+".log");
    for(size_t i=0;i<input.size();++i) {
      const auto start=Measure::now();logger.log("%\n",input[i]);samples[i]=Measure::now()-start;
    }
    dropped=logger.dropped_records();
  } // Join and drain; completion rate includes file close, but does not imply fsync.
  const auto elapsed=Measure::now()-window;
  std::ifstream file(name+".log");
  const std::string actual((std::istreambuf_iterator<char>(file)),{});
  Measure::require(dropped==0,"logger dropped records; invalid performance run");
  Measure::require(actual==expected+"[LOGGER] dropped_records=0\n","logger output bytes differ");
  Measure::output(name,samples,elapsed,expected.size(),dropped);
}
int main(int argc,char**) {
  try {
    std::vector<std::string> input;input.reserve(2000);
    for(size_t i=0;i<2000;++i)input.push_back(std::to_string(i)+":"+std::string(i%2?8192:128,char('A'+i%26)));
    std::ofstream source("logger_input.txt");for(const auto& line:input)source<<line<<'\n';source.close();
    Measure::clockOverhead();
    std::cout<<"NOTE OptLogger aliases Logger; controls use identical 128/8192-character payloads and require exact output. Latency=log call; rate=records drained and file closed per second, not durable journal throughput.\n";
    if(argc>1) {
      benchmark<OptCommon::OptLogger>("logger_control_b",input);
      benchmark<Common::Logger>("logger_control_a",input);
    } else {
      benchmark<Common::Logger>("logger_control_a",input);
      benchmark<OptCommon::OptLogger>("logger_control_b",input);
    }
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
