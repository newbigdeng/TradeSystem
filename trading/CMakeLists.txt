
file(GLOB SOURCES "*/*.cpp")

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/trading)

add_library(libtrading STATIC ${SOURCES} strategy/risk_manager.cpp)
