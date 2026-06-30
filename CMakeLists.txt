
CMAKE_MINIMUM_REQUIRED( VERSION 3.10.1 )
project(FastOrderBook)

set(CMAKE_CXX_STANDARD 20)

add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

option(BUILD_TEST "Build benchmarks and unit tests" OFF)
if(BUILD_TEST)
    find_package(benchmark REQUIRED)
    find_package(GTest REQUIRED)

    add_executable(testorderbook ${CMAKE_CURRENT_SOURCE_DIR}/testorderbook.cpp)
    target_link_libraries(testorderbook PRIVATE orderbook benchmark::benchmark)

    add_executable(testdynamicorderbook ${CMAKE_CURRENT_SOURCE_DIR}/testdynamicorderbook.cpp)
    target_link_libraries(testdynamicorderbook PRIVATE orderbook GTest::gtest GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(testdynamicorderbook)
endif()
