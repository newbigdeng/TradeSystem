cmake_minimum_required(VERSION 3.16)

project(LowLatencyApp LANGUAGES CXX)

add_library(trade_build_options INTERFACE)
target_compile_features(trade_build_options INTERFACE cxx_std_20)
target_compile_options(trade_build_options INTERFACE -Wall -Wextra -Werror -Wpedantic)
set(TRADE_SANITIZER "" CACHE STRING "address or thread; empty for ordinary builds")
if(TRADE_SANITIZER STREQUAL "address")
  target_compile_options(trade_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(trade_build_options INTERFACE -fsanitize=address,undefined)
elseif(TRADE_SANITIZER STREQUAL "thread")
  target_compile_options(trade_build_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(trade_build_options INTERFACE -fsanitize=thread)
elseif(NOT TRADE_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unsupported TRADE_SANITIZER")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
link_libraries(trade_build_options)



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
target_link_libraries(exchange_main PUBLIC ${LIBS})

add_executable(trading_main trading/trading_main.cpp)
target_link_libraries(trading_main PUBLIC ${LIBS})

add_executable(logger_benchmark benchmarks/logger_benchmark.cpp)
target_link_libraries(logger_benchmark PUBLIC ${LIBS})

add_executable(release_benchmark benchmarks/release_benchmark.cpp)
target_link_libraries(release_benchmark PUBLIC ${LIBS})

add_executable(hash_benchmark benchmarks/hash_benchmark.cpp)
target_link_libraries(hash_benchmark PUBLIC ${LIBS})

# Exercise both market-data groups on the configured interface, without root.
include(CTest)
if(BUILD_TESTING)
  add_executable(multicast_interface_test record/00_multicast_interface/multicast_interface_test.cpp)
  target_link_libraries(multicast_interface_test PRIVATE libcommon pthread)
  add_test(NAME multicast_interface COMMAND multicast_interface_test)
  set_tests_properties(multicast_interface PROPERTIES TIMEOUT 20)
  add_test(NAME multicast_invalid_interface COMMAND multicast_interface_test invalid)
  set_tests_properties(multicast_invalid_interface PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
endif()


if(BUILD_TESTING)
  add_executable(queue_test record/01_reliability/queue_test.cpp)
  target_link_libraries(queue_test PRIVATE libcommon pthread)
  add_test(NAME reliability_queue COMMAND queue_test)
  set_tests_properties(reliability_queue PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
  add_executable(reliability_test record/01_reliability/reliability_test.cpp)
  target_link_libraries(reliability_test PRIVATE libcommon pthread)
  add_test(NAME reliability_tcp COMMAND reliability_test tcp)
  set_tests_properties(reliability_tcp PROPERTIES TIMEOUT 30)
endif()
