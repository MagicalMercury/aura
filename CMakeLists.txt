cmake_minimum_required(VERSION 4.1)

# 深度调试选项：开启后切换到 MSYS2 CLANG64 的 clang 并启用 AddressSanitizer
# 默认关闭，使用环境变量配置的 g++（UCRT64 GCC，即当前常规模式）
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

project(Aura)

# 默认 Debug 编译
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -Os -g -DNDEBUG")
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
    message(STATUS ">>> 常规模式：UCRT64 g++")
endif()

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
    src/Sema/SemTypeUtils.cpp
    src/Sema/Assignability.cpp
    src/Sema/GenericSubstitution.cpp
    src/Sema/TypeResolution.cpp
    src/Sema/CanonicalPropagation.cpp
    src/Sema/Checker/DeclChecker.cpp
    src/Sema/Checker/TypeResolver.cpp
    src/Sema/Checker/BodyChecker.cpp
    src/Sema/Checker/StmtChecker.cpp
    src/Sema/Checker/StmtFlow.cpp
    src/Sema/Checker/StmtSync.cpp
    src/Sema/Checker/CallGraph.cpp
    src/Sema/Checker/ExprInfer.cpp
    src/Sema/Checker/CallInfer.cpp
    src/Sema/Checker/ExprInferMisc.cpp
    src/CodeGen/CodeGen.cpp
    src/CodeGen/TypeMap.cpp
    src/CodeGen/DeclGen.cpp
    src/CodeGen/DeclFun.cpp
    src/CodeGen/DeclTParams.cpp
    src/CodeGen/StmtGen.cpp
    src/CodeGen/ExprGen.cpp
    src/CodeGen/ExprBinary.cpp
    src/CodeGen/ExprCall.cpp
    src/CodeGen/ExprMethodCall.cpp
    src/CodeGen/ExprAccess.cpp
    src/CodeGen/ExprClosure.cpp
    src/CodeGen/ExprClosureArgs.cpp
    src/CodeGen/ExprClosureCallableObj.cpp
    src/CodeGen/ExprClosureCaptures.cpp
    src/CodeGen/ExprClosureOldPath.cpp
    src/CodeGen/CoroDecide.cpp
    src/CodeGen/StmtLet.cpp
    src/CodeGen/UnionBoxing.cpp
    src/CodeGen/StmtControl.cpp
    src/CodeGen/StmtTry.cpp
    src/CodeGen/StmtSync.cpp
    src/CodeGen/StmtSpawn.cpp
    src/CodeGen/StmtMatch.cpp
    src/Module/ModuleManager.cpp
    src/Diag/DiagnosticEngine.cpp
)

set(AURA_HEADERS
    src/ASTWalker.h
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
    src/Sema/BuiltinRegistry.h
    src/CodeGen/CodeGen.h
    src/Module/ModuleManager.h
    src/Diag/Diagnostic.h
    src/Diag/DiagnosticEngine.h
)

add_executable(aurac ${AURA_SOURCES} ${AURA_HEADERS})

target_include_directories(aurac PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ASAN 模式下：编译后将 ASAN 动态运行时 DLL 复制到 aurac 输出目录
# 这样 aurac.exe 同目录就有 libclang_rt.asan_dynamic-x86_64.dll，运行时自动找到
if(ENABLE_ASAN)
    set(ASAN_DLL "C:/msys64/clang64/bin/libclang_rt.asan_dynamic-x86_64.dll")
    add_custom_command(TARGET aurac POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ASAN_DLL}"
            "$<TARGET_FILE_DIR:aurac>"
        COMMENT "Copying ASAN runtime DLL to output directory")
endif()
