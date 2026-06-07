cmake_minimum_required(VERSION 3.20)
project(aura_rt LANGUAGES CXX)

# 默认 Debug 编译
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O2 -g -DNDEBUG")

add_library(aura_rt STATIC
    types.cpp
    gc.cpp
    builtin/io.cpp
)

target_include_directories(aura_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_rt PUBLIC stdc++exp)   # <filesystem> (某些 GCC 版本需要)
