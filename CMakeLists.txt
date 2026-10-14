cmake_minimum_required(VERSION 3.16)

project(LowLatencyApp LANGUAGES CXX)

add_library(trade_build_options INTERFACE)
target_compile_features(trade_build_options INTERFACE cxx_std_20)
target_compile_options(trade_build_options INTERFACE -Wall -Wextra -Werror -Wpedantic)
set(TRADE_PERF_TRACE_EVERY "1" CACHE STRING "Diagnostic timing records: 0 off, 1 full, N one in N calls per thread/site")
if(NOT TRADE_PERF_TRACE_EVERY MATCHES "^(0|[1-9][0-9]*)$" OR TRADE_PERF_TRACE_EVERY GREATER 1000000)
  message(FATAL_ERROR "TRADE_PERF_TRACE_EVERY must be an integer from 0 to 1000000")
endif()
target_compile_definitions(trade_build_options INTERFACE TRADE_PERF_TRACE_EVERY=${TRADE_PERF_TRACE_EVERY})
set(TRADE_SANITIZER "" CACHE STRING "address or thread; empty for ordinary builds")
if(TRADE_SANITIZER STREQUAL "address")
  target_compile_options(trade_build_options INTERFACE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(trade_build_options INTERFACE -O1 -fsanitize=address,undefined)
elseif(TRADE_SANITIZER STREQUAL "thread")
  target_compile_options(trade_build_options INTERFACE -O1 -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(trade_build_options INTERFACE -O1 -fsanitize=thread)
elseif(NOT TRADE_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unsupported TRADE_SANITIZER")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
link_libraries(trade_build_options)



# Glaze 只供启动配置与配置测试使用；保留上游头文件和许可，离线构建。
add_library(trade_config INTERFACE)
target_include_directories(trade_config SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party/glaze/include)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/config)
foreach(config_name IN ITEMS exchange client)
  configure_file(config/${config_name}.json ${CMAKE_CURRENT_BINARY_DIR}/config/${config_name}.json COPYONLY)
endforeach()

add_subdirectory(common)
add_subdirectory(exchange)
add_subdirectory(trading)
add_subdirectory(benchmarks)

list(APPEND LIBS libexchange)
list(APPEND LIBS libtrading)
list(APPEND LIBS libcommon)
list(APPEND LIBS pthread)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)
include_directories(${PROJECT_SOURCE_DIR}/trading)

add_executable(exchange_main exchange/exchange_main.cpp)
target_link_libraries(exchange_main PUBLIC ${LIBS} trade_config)

add_executable(trading_main trading/trading_main.cpp)
target_link_libraries(trading_main PUBLIC ${LIBS} trade_config)

add_executable(logger_benchmark benchmarks/logger_benchmark.cpp)
target_link_libraries(logger_benchmark PUBLIC ${LIBS})

add_executable(release_benchmark benchmarks/release_benchmark.cpp)
target_link_libraries(release_benchmark PUBLIC ${LIBS})

add_executable(hash_benchmark benchmarks/hash_benchmark.cpp)
target_link_libraries(hash_benchmark PUBLIC ${LIBS})

# 在指定接口验证两路行情组播，无需 root。
include(CTest)
if(BUILD_TESTING)
  add_executable(multicast_interface_test tests/00_multicast_interface/multicast_interface_test.cpp)
  target_link_libraries(multicast_interface_test PRIVATE libcommon pthread)
  add_test(NAME multicast_interface COMMAND multicast_interface_test)
  set_tests_properties(multicast_interface PROPERTIES TIMEOUT 20)
  add_test(NAME multicast_invalid_interface COMMAND multicast_interface_test invalid)
  set_tests_properties(multicast_invalid_interface PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
endif()


if(BUILD_TESTING)
  add_executable(queue_test tests/01_reliability/queue_test.cpp)
  target_link_libraries(queue_test PRIVATE libcommon pthread)
  add_test(NAME reliability_queue COMMAND queue_test)
  set_tests_properties(reliability_queue PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
  add_executable(reliability_test tests/01_reliability/reliability_test.cpp)
  target_link_libraries(reliability_test PRIVATE libcommon pthread)
  add_test(NAME reliability_tcp COMMAND reliability_test tcp)
  set_tests_properties(reliability_tcp PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
  add_test(NAME reliability_pool COMMAND reliability_test pool)
endif()

if(BUILD_TESTING)
  add_test(NAME reliability_fifo COMMAND reliability_test fifo)
endif()

if(BUILD_TESTING)
 add_executable(book_model_test tests/01_reliability/book_model_test.cpp)
 target_link_libraries(book_model_test PRIVATE libexchange libtrading libcommon pthread)
 add_test(NAME reliability_book COMMAND book_model_test)
 add_test(NAME reliability_unordered_book COMMAND book_model_test unordered)
 set_tests_properties(reliability_book reliability_unordered_book PROPERTIES TIMEOUT 180)
endif()

if(BUILD_TESTING)
 add_executable(protocol_test tests/01_reliability/protocol_test.cpp)
 target_link_libraries(protocol_test PRIVATE libcommon pthread)
 add_test(NAME reliability_protocol COMMAND protocol_test)
 add_executable(control_probe tests/01_reliability/control_probe.cpp)
 target_link_libraries(control_probe PRIVATE libtrading libexchange libcommon pthread)
 add_test(NAME reliability_order_state COMMAND control_probe state)
 add_test(NAME reliability_final_risk_gate COMMAND control_probe admission)
 set_tests_properties(reliability_final_risk_gate PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
 add_executable(recovery_test tests/01_reliability/recovery_test.cpp)
 target_link_libraries(recovery_test PRIVATE libcommon pthread)
 add_test(NAME reliability_recovery COMMAND recovery_test)
endif()

if(BUILD_TESTING)
 add_test(NAME reliability_features COMMAND control_probe feature)
 add_test(NAME reliability_thread_lifetime COMMAND control_probe lifecycle)
endif()

if(BUILD_TESTING)
 add_executable(journal_test tests/01_reliability/journal_test.cpp)
 target_link_libraries(journal_test PRIVATE libexchange libcommon pthread)
 add_test(NAME reliability_journal COMMAND journal_test)
endif()

if(BUILD_TESTING)
 add_test(NAME reliability_concurrent_admission COMMAND control_probe concurrent)
 set_tests_properties(reliability_concurrent_admission PROPERTIES TIMEOUT 120)
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(measurement_component_benchmark tests/02_reproducible_measurement/component_benchmark.cpp)
  target_link_libraries(measurement_component_benchmark PRIVATE libtrading libexchange libcommon pthread)
  add_test(NAME measurement_analysis COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/02_reproducible_measurement/analysis_test.py)
  add_test(NAME measurement_generator COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/02_reproducible_measurement/load_generator_test.py)
  set_tests_properties(measurement_generator PROPERTIES TIMEOUT 20)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/measurement-smoke)
  foreach(probe IN ITEMS release_benchmark logger_benchmark hash_benchmark measurement_component_benchmark)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/measurement-smoke/${probe})
    add_test(NAME measurement_${probe} COMMAND ${probe})
    set_tests_properties(measurement_${probe} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/measurement-smoke/${probe} TIMEOUT 90)
  endforeach()
endif()

# 固定容量队列、对象池契约及 FIFO 背压测试。
if(BUILD_TESTING)
  add_executable(queue_pool_regression tests/06_queue_pool/queue_pool_regression.cpp)
  target_compile_definitions(queue_pool_regression PRIVATE EXPECT_POW2 EXPECT_COMPACT_POOL)
  target_link_libraries(queue_pool_regression PRIVATE libcommon pthread)
  foreach(probe IN ITEMS queue pool fifo)
    add_test(NAME queue_pool_${probe} COMMAND queue_pool_regression ${probe})
    set_tests_properties(queue_pool_${probe} PROPERTIES TIMEOUT 90)
  endforeach()
endif()

# 不触发接收回调，单独推进积压回报的发送。
if(BUILD_TESTING)
  add_executable(network_progress_test tests/07_e2e_latency/network_progress_test.cpp)
  target_link_libraries(network_progress_test PRIVATE libexchange libcommon pthread)
  add_test(NAME network_flush_progress COMMAND network_progress_test flush)
  set_tests_properties(network_flush_progress PROPERTIES TIMEOUT 30)
endif()

# 恢复批次应用完成后才开放策略；过期标记不能重新启用旧行情。
if(BUILD_TESTING)
  add_test(NAME reliability_recovery_apply COMMAND control_probe recovery)
  set_tests_properties(reliability_recovery_apply PROPERTIES TIMEOUT 30)
  add_executable(book_recovery_benchmark tests/08_book_recovery/optimization_benchmark.cpp)
  target_link_libraries(book_recovery_benchmark PRIVATE libexchange libcommon pthread)
  add_executable(recovery_commit_benchmark tests/08_book_recovery/commit_benchmark.cpp)
  target_compile_definitions(recovery_commit_benchmark PRIVATE TRADE_SIMPLE_RECOVERY)
  target_link_libraries(recovery_commit_benchmark PRIVATE libtrading libexchange libcommon pthread)
endif()

if(BUILD_TESTING)
  add_executable(wire_decode_benchmark tests/08_book_recovery/wire_decode_benchmark.cpp)
  target_link_libraries(wire_decode_benchmark PRIVATE libcommon pthread)
endif()

# 启动配置契约：必填项、数值范围、网卡、策略及两路组播。
if(BUILD_TESTING)
  add_executable(runtime_config_test tests/09_runtime_config/config_test.cpp)
  target_link_libraries(runtime_config_test PRIVATE trade_config pthread)
  add_test(NAME runtime_config_contract COMMAND runtime_config_test
    ${PROJECT_SOURCE_DIR}/config/exchange.json ${PROJECT_SOURCE_DIR}/config/client.json)
  set_tests_properties(runtime_config_contract PROPERTIES TIMEOUT 30)
endif()

# 策略事件语义与分派方式对照；性能基准不加入自动回归。
if(BUILD_TESTING)
  add_executable(strategy_behavior_test tests/09_runtime_config/strategy_behavior_test.cpp)
  target_link_libraries(strategy_behavior_test PRIVATE libtrading libexchange libcommon pthread)
  add_test(NAME strategy_behavior COMMAND strategy_behavior_test)
  set_tests_properties(strategy_behavior PROPERTIES TIMEOUT 30)
  add_executable(strategy_dispatch_benchmark tests/09_runtime_config/dispatch_benchmark.cpp
    tests/09_runtime_config/dispatch_factory.cpp)
  target_compile_options(strategy_dispatch_benchmark PRIVATE -fno-lto)
  target_link_options(strategy_dispatch_benchmark PRIVATE -fno-lto)
endif()

# 通道边界：坏周期快照不打断实时流，快照控制帧不能进入增量队列。
if(BUILD_TESTING)
  add_executable(recovery_channel_test tests/08_book_recovery/recovery_channel_test.cpp)
  target_link_libraries(recovery_channel_test PRIVATE libtrading libexchange libcommon pthread)
  add_test(NAME recovery_channel COMMAND recovery_channel_test)
  set_tests_properties(recovery_channel PROPERTIES TIMEOUT 30)
  add_executable(client_book_benchmark tests/08_book_recovery/client_book_benchmark.cpp)
  target_link_libraries(client_book_benchmark PRIVATE libtrading libexchange libcommon pthread)
endif()


# 无锁队列定向契约与语义检查；性能基准不加入自动回归。
if(BUILD_TESTING)
  add_executable(queue_capacity_contract record/07_queue_pool_consumer/capacity_contract.cpp)
  target_link_libraries(queue_capacity_contract PRIVATE libcommon pthread)
  add_test(NAME queue_capacity_contract COMMAND queue_capacity_contract)
  set_tests_properties(queue_capacity_contract PROPERTIES TIMEOUT 10)

  add_executable(queue_semantics_test record/07_queue_pool_consumer/queue_semantics.cpp)
  target_link_libraries(queue_semantics_test PRIVATE libcommon pthread)
  add_test(NAME queue_semantics COMMAND queue_semantics_test)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/queue-semantics)
  set_tests_properties(queue_semantics PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/queue-semantics TIMEOUT 60)

  add_executable(queue_hotpath_benchmark record/07_queue_pool_consumer/queue_benchmark.cpp)
  target_link_libraries(queue_hotpath_benchmark PRIVATE libcommon pthread)
endif()

# 消费状态、错误退出、定序背压和日志字节契约；基准不加入自动回归。
if(BUILD_TESTING)
  add_executable(consumer_hotpath_test record/07_queue_pool_consumer/consumer_regression_test.cpp)
  target_link_libraries(consumer_hotpath_test PRIVATE libexchange libtrading libcommon pthread)
  add_test(NAME consumer_hotpath_contract COMMAND consumer_hotpath_test)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/consumer-hotpath)
  set_tests_properties(consumer_hotpath_contract PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/consumer-hotpath TIMEOUT 90)
  add_executable(consumer_hotpath_benchmark record/07_queue_pool_consumer/consumer_benchmark.cpp)
  target_link_libraries(consumer_hotpath_benchmark PRIVATE libexchange libtrading libcommon pthread)
  add_executable(pool_layout_benchmark record/07_queue_pool_consumer/pool_benchmark.cpp)
  target_link_libraries(pool_layout_benchmark PRIVATE libcommon pthread)
endif()

# 交易引擎真实行情消费：合法簿状态、恢复标记与非法品种仍明确失败。
if(BUILD_TESTING)
  add_executable(trade_guard_contract_test record/07_queue_pool_consumer/trade_guard_regression.cpp)
  target_link_libraries(trade_guard_contract_test PRIVATE libtrading libexchange libcommon pthread)
  add_test(NAME trade_engine_consumer_contract COMMAND trade_guard_contract_test)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/trade-guard)
  set_tests_properties(trade_engine_consumer_contract PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/trade-guard TIMEOUT 90)
  add_executable(trade_guard_benchmark record/07_queue_pool_consumer/trade_guard_benchmark.cpp)
  target_link_libraries(trade_guard_benchmark PRIVATE libtrading libexchange libcommon pthread)
endif()
