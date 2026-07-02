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

# Each test is a standalone binary using the tiny zero-dep framework in tests/.
foreach(t test_orderbook test_occupancy test_flat_id_map test_alloc_audit)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(${t} PRIVATE orderbook)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(ob_bench bench/bench.cpp)
target_link_libraries(ob_bench PRIVATE orderbook)
target_compile_options(ob_bench PRIVATE -O3)
