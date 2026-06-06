cmake_minimum_required(VERSION 3.20)
project(aura_rt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

add_library(aura_rt STATIC
    types.cpp
    gc.cpp
    builtin/io.cpp
)

target_include_directories(aura_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_rt PUBLIC stdc++exp)   # <filesystem> (某些 GCC 版本需要)
