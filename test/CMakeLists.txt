# ============================================================
# test/CMakeLists.txt — Aura 编译器单元测试
#
# 构建一个独立可执行文件 aura_tests，包含全部编译器源码
# （不含 main.cpp）与所有 TEST() 用例，注册到 CTest。
#
# 目录组织（按编译器管道阶段）：
#   framework/    测试基础设施（框架 / 入口 / 辅助工具）
#   lexer/        词法分析单元测试
#   parser/       语法分析单元测试
#   ast/          AST 单元测试
#   sema/         语义分析单元测试
#   codegen/      代码生成单元测试
#   diag/         诊断引擎单元测试
#   module/       模块管理单元测试
#   integration/  端到端集成测试（example 回归）
#
# 用法：
#   ctest --test-dir build            # 运行全部测试
#   ./build/aura_tests.exe --filter=Lexer   # 运行指定套件（精确匹配套件名）
# ============================================================
cmake_minimum_required(VERSION 4.1)
project(AuraTests)

# 编译器源码（不含 main.cpp，测试自带 framework/test_main.cpp 入口）
set(AURA_LIB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/TokType.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ASTPrinter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Parser/Parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Parser/DeclParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Parser/StmtParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Parser/ExprParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Parser/TypeParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/SemType.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/SymbolTable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/SemAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/SemTypeUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Assignability.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/GenericSubstitution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/TypeResolution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/CanonicalPropagation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/DeclChecker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/TypeResolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/BodyChecker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/StmtChecker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/StmtFlow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/StmtSync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/ExprInfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/CallInfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/ExprInferMisc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/CodeGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/TypeMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclFun.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclTParams.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprBinary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprCall.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprMethodCall.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprAccess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprClosure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/CoroDecide.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtLet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/UnionBoxing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtControl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtTry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtSync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtSpawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtMatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Module/ModuleManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Diag/DiagnosticEngine.cpp
)

# 测试用例（按模块分组，每个文件一个套件，保持小文件）
set(AURA_TEST_SOURCES
    # ---- 测试基础设施 ----
    framework/test_main.cpp

    # ---- 词法分析 ----
    lexer/test_toktype.cpp
    lexer/test_lexer.cpp

    # ---- 语法分析 ----
    parser/test_parser_decl.cpp
    parser/test_parser_expr.cpp
    parser/test_parser_stmt.cpp
    parser/test_parser_error.cpp

    # ---- AST ----
    ast/test_ast.cpp

    # ---- 语义分析 ----
    sema/test_sema_types.cpp
    sema/test_sema_functions.cpp
    sema/test_sema_controlflow.cpp
    sema/test_sema_concurrency.cpp
    sema/test_sema_interfaces.cpp
    sema/test_sema_generics.cpp
    sema/test_sema_optional.cpp
    sema/test_sema_numeric.cpp
    sema/test_sema_tuple.cpp
    sema/test_sema_modules.cpp
    sema/test_sema_channel.cpp
    sema/test_sema_lock.cpp
    sema/test_sema_list_optional.cpp
    sema/test_sema_record.cpp
    sema/test_sema_record_optional.cpp
    sema/test_sema_spawn.cpp
    sema/test_sema_iterator.cpp
    sema/test_sema_functype.cpp
    sema/test_sema_crashguard.cpp

    # ---- 代码生成 ----
    codegen/test_codegen.cpp

    # ---- 诊断引擎 ----
    diag/test_diag.cpp

    # ---- 模块管理 ----
    module/test_module.cpp

    # ---- 端到端集成测试 ----
    integration/test_examples.cpp
)

add_executable(aura_tests ${AURA_TEST_SOURCES} ${AURA_LIB_SOURCES})

target_include_directories(aura_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 运行期切换到项目根目录，使 ModuleManager 能找到 builtins/*.aurai
target_compile_definitions(aura_tests PRIVATE
    AURA_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# 线程库（协程/并发测试）
find_package(Threads REQUIRED)
target_link_libraries(aura_tests PRIVATE Threads::Threads)

# 启用 CTest 注册
enable_testing()

# 注册到 CTest
add_test(NAME aura_tests COMMAND aura_tests)
