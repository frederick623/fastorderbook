
CMAKE_MINIMUM_REQUIRED( VERSION 3.10.1 )
project(FastOrderBook)

set(CMAKE_CXX_STANDARD 20)

add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

option(BUILD_TEST "Build documentation and samples" OFF)
if(BUILD_TEST)
    find_package(benchmark REQUIRED)

    add_executable(testorderbook ${CMAKE_CURRENT_SOURCE_DIR}/test/testorderbook.cpp)
    target_link_libraries(testorderbook PRIVATE orderbook benchmark::benchmark)
endif()
