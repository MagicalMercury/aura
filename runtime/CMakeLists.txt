cmake_minimum_required(VERSION 3.20)

# 深度调试选项：开启后切换到 MSYS2 CLANG64 的 clang 并启用 AddressSanitizer
# 默认关闭，使用环境变量配置的 g++（即当前常规模式）
option(ENABLE_ASAN "启用 AddressSanitizer 深度调试（切换到 CLANG64 clang）" OFF)

if(ENABLE_ASAN)
    # 深度调试：CLANG64 的 clang 带 ASAN 运行时，UCRT64 的 GCC 不支持 ASAN
    set(CMAKE_C_COMPILER "C:/msys64/clang64/bin/clang.exe")
    set(CMAKE_CXX_COMPILER "C:/msys64/clang64/bin/clang++.exe")
else()
    # 常规模式：使用环境变量配置的 g++（UCRT64 GCC）
    set(CMAKE_C_COMPILER "C:/msys64/ucrt64/bin/gcc.exe")
    set(CMAKE_CXX_COMPILER "C:/msys64/ucrt64/bin/g++.exe")
endif()

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

# AddressSanitizer 配置（仅在 ENABLE_ASAN 开启时生效）
# 注意：clang++ 默认调用 UCRT64 的 GNU ld，但 GNU ld 不会自动链接 ASAN 运行时库。
# 必须用 -fuse-ld=lld 强制 clang 使用 clang64 自带的 ld.lld，后者能正确发现并链接
# libclang_rt.asan_dynamic-x86_64.dll.a 等运行时库。
if(ENABLE_ASAN)
    message(STATUS ">>> 深度调试模式：CLANG64 clang + lld + AddressSanitizer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fuse-ld=lld")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address -fuse-ld=lld")
else()
    message(STATUS ">>> 常规模式：环境变量配置的 g++")
endif()

add_library(aura_rt STATIC
    types.cpp
    gc/gc.cpp
    gc/alloc.cpp
    gc/tlab.cpp
    gc/roots.cpp
    gc/safepoint.cpp
    gc/mark_sweep.cpp
    gc/parallel_mark.cpp
    gc/compact.cpp
    gc/los.cpp
    task.cpp
    thread_pool.cpp
    builtin/io.cpp
    builtin/string.cpp
    builtin/mutex.cpp
    win_iocp.cpp
)

target_include_directories(aura_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_rt PUBLIC stdc++exp)   # <filesystem> (某些 GCC 版本需要)
