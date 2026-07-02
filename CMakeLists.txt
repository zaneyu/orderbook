cmake_minimum_required(VERSION 3.16)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Header-only engine.
add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

add_executable(ob_tests tests/test_orderbook.cpp)
target_include_directories(ob_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(ob_tests PRIVATE orderbook)
target_compile_options(ob_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME ob_tests COMMAND ob_tests)

add_executable(ob_bench bench/bench.cpp)
target_link_libraries(ob_bench PRIVATE orderbook)
target_compile_options(ob_bench PRIVATE -O3)
