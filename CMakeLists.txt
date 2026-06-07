cmake_minimum_required(VERSION 4.1)

# 使用 MSYS2 UCRT64 的 GCC 工具链
set(CMAKE_C_COMPILER "C:/msys64/ucrt64/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "C:/msys64/ucrt64/bin/g++.exe")

project(Aura)

# 默认 Debug 编译
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -Os -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O2 -g -DNDEBUG")

# 禁用 C++20 模块自动扫描（本项目不使用模块）
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(AURA_SOURCES
    src/main.cpp
    src/TokType.cpp
    src/Lexer.cpp
    src/ASTPrinter.cpp
    src/Parser/Parser.cpp
    src/Parser/DeclParser.cpp
    src/Parser/StmtParser.cpp
    src/Parser/ExprParser.cpp
    src/Parser/TypeParser.cpp
    src/Sema/SemType.cpp
    src/Sema/SymbolTable.cpp
    src/Sema/SemAnalyzer.cpp
    src/Sema/Checker/DeclChecker.cpp
    src/Sema/Checker/StmtChecker.cpp
    src/Sema/Checker/ExprInfer.cpp
    src/CodeGen/CodeGen.cpp
    src/CodeGen/TypeMap.cpp
    src/CodeGen/DeclGen.cpp
    src/CodeGen/StmtGen.cpp
    src/CodeGen/ExprGen.cpp
    src/CodeGen/CoroDecide.cpp
    src/Module/ModuleManager.cpp
)

set(AURA_HEADERS
    src/TokType.h
    src/Token.h
    src/Lexer.h
    src/ASTPrinter.h
    src/Parser.h
    src/AST/ASTNode.h
    src/AST/Expr.h
    src/AST/Stmt.h
    src/AST/Type.h
    src/Sema/SemType.h
    src/Sema/Symbol.h
    src/Sema/SymbolTable.h
    src/Sema/SemAnalyzer.h
    src/CodeGen/CodeGen.h
    src/Module/ModuleManager.h
)

add_executable(aurac ${AURA_SOURCES} ${AURA_HEADERS})

target_include_directories(aurac PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
