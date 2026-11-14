# ============================================================
# test/CMakeLists.txt — Aura 编译器单元测试
#
# 构建策略（2026-11-08）：
#   · aura_tests            兼容总入口：链接全部测试 OBJECT，仍可直接运行；
#   · aura_tests_<category> 按类别拆分：rt/lexer/parser/ast/sema/codegen/diag/module/integration；
#   · 编译器/运行时核心为 OBJECT 库 aura_test_core，所有 exe 共用，只编一次；
#   · 测试源也是 OBJECT 库，总入口与分类 exe 共用，不重复编译。
#
# 常用命令：
#   cmake --build test/build                    # 构建全部
#   cmake --build test/build --target aura_tests_sema
#   ./test/build/aura_tests_sema.exe            # 只跑 sema
#   ctest --test-dir test/build -j4             # 并行跑 9 个类别
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/CallGraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/ExprInfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/CallInfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Sema/Checker/ExprInferMisc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/MetaCollect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/MetaEmit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/CodeGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/TypeMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclFun.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclEnum.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/DeclTParams.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprBinary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprCall.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprMethodCall.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprAccess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprClosure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprClosureArgs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprClosureCallableObj.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprClosureCaptures.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/ExprClosureOldPath.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/CoroDecide.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtLet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/UnionBoxing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtControl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtTry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtSync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtSpawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CodeGen/StmtMatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Module/ExpandConst.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Module/CompilationDriver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Module/ModuleManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CompileTime/DeclIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CompileTime/Evaluator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CompileTime/EvalPass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Diag/DiagnosticEngine.cpp
)

# 测试用例（按模块分组，每个文件一个套件，保持小文件）
# runtime sources (feature-14 P1: SyncContext unit tests call runtime directly)
# Kept in sync with the aura_rt STATIC source list in runtime/CMakeLists.txt.
set(AURA_RT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/task.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/thread_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/win_iocp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/gc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/gc_interrupt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/alloc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/tlab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/roots.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/safepoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/mark_sweep.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/parallel_mark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/compact.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/los.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/builtin/io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/builtin/string.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/builtin/mutex.cpp
)

# Platform-specific interrupt primitive (Linux SIGURG / Windows APC + IOCP fake completion)
if(WIN32)
    list(APPEND AURA_RT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/safepoint_interrupt_win.cpp)
else()
    list(APPEND AURA_RT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gc/safepoint_signal_linux.cpp)
endif()
set(AURA_TEST_SOURCES
    # ---- 测试基础设施 ----
    framework/test_main.cpp

    # ---- runtime (feature-14 P1: SyncContext domain stack) ----
    rt/test_sync_context.cpp

    # ---- runtime (bug-86 回归：STW 死锁 + concat_multi 并发 GC 越界) ----
    rt/test_gc_bug86.cpp

    # ---- runtime（feature-20 批 3：enum 形态 1/2 的 GC 语义 —— compact(All) 后仍有效）----
    rt/test_enum_gc.cpp

    # ---- runtime (bug-89 回归：globalRoots_ 更新步骤 3 的 isGCAddress 守卫) ----
    rt/test_gc_bug89.cpp

    # ---- runtime (bug-110 回归：mark 阶段 3 处 isGCAddress 守卫 —— 全局根 / oomError 四指针 / 中页全局根) ----
    rt/test_gc_bug110.cpp

    # ---- runtime (bug-111 回归：compact() 入口「所有 TLAB 已 flush」不变量自检 hasDirtyTlabs()) ----
    rt/test_gc_bug111.cpp

    # ---- runtime (bug-112 回归：intern L1 缓存 key 改内联字节副本 + >32 不进 L1) ----
    rt/test_intern_cache.cpp

    rt/test_error_stack.cpp        # feature-18 P1
    rt/test_logical_stack.cpp      # feature-18 P1
    rt/test_logical_stack_p4a.cpp  # feature-18 P4a 批 4（T1–T3：FrameGuard 配平 / 真递归 300 帧不变量 / setFrameLine 只改栈顶）
    rt/test_coro_snapshot_p4b1.cpp # feature-18 P4b-1 批（T4–T8 / T13–T15：快照往返、64 截断、空栈、baseDepth 随快照（含跨线程）、覆盖非 push、无 entry、entry 生命周期 + B0/B1 联动）
    rt/test_coro_snapshot_wiring_p4b2.cpp # feature-18 P4b-2 批（B4/B5 **接线实证**：channel send/recv + task_awaiter 挂起快照；close/try_flush 的 scoped 分派、final 短链、run_to_completion 创建点恢复。只编译不跑）
    rt/test_error_format.cpp       # feature-18 P5（诊断输出：formatError 首行 + 逻辑栈解码/查表/折叠/深度上限/退化；只编译不跑）
    rt/test_io.cpp                 # bug-97（read_file 错误分支：非 ASAN 判据；走 read_file_sync 规避 bug-102）
    rt/test_thread_pool_error.cpp  # bug-103（sync_thread_context waitAndRethrow 传播 + 幂等）

    # ---- 词法分析 ----
    lexer/test_toktype.cpp
    lexer/test_lexer.cpp

    # ---- 语法分析 ----
    parser/test_parser_decl.cpp
    parser/test_parser_expr.cpp
    parser/test_parser_stmt.cpp
    parser/test_parser_error.cpp
    parser/test_parser_enum.cpp
    parser/test_parser_hash.cpp

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
    sema/test_sema_enum.cpp          # feature-20 批 2/4（enum Sema 面：类型/约束/深只读/==/match）

    # ---- 代码生成（test_codegen.cpp 按主题拆分，2026-09-06）----
    codegen/test_codegen_coro.cpp
    codegen/test_codegen_struct_type.cpp
    codegen/test_codegen_generic.cpp
    codegen/test_codegen_optional_union.cpp
    codegen/test_codegen_iface_view.cpp
    codegen/test_codegen_closure.cpp
    codegen/test_codegen_concurrency_gc.cpp
    codegen/test_codegen_try.cpp             # feature-18 P2
    codegen/test_codegen_meta.cpp            # feature-18 P3（符号元数据表 + aura.meta；⑩ 为 long-running：真调 g++）
    codegen/test_codegen_frames_p4a.cpp      # feature-18 P4a 批 4（T9–T12 编号 + C1–C11/C-新/C-新2 生成码形态）
    codegen/test_codegen_frames_p4b1.cpp     # feature-18 P4b-1（C6–C8 匿名帧四处注入 + 匿名帧区形态 + 门控 OFF 零注入）
    codegen/test_codegen_spawn_call.cpp      # bug-98（spawn <call> 调用形态真运行：真编译+真链接+真执行，collector ON/OFF 双路径）
    codegen/test_codegen_spawn_let_coro.cpp  # bug-93（spawn 体内 let 绑定协程调用：co_await 落 lambda 内；真编译+真链接+真运行，collector ON/OFF）
    codegen/test_codegen_path_arg_lifetime.cpp  # bug-102（惰性协程按引用值类型实参：A 物化提升形态断言 + B 长路径按值真运行；collector ON/OFF）
    codegen/test_codegen_sync_thread.cpp        # bug-103（sync thread 块尾 waitAndRethrow：生成码断言 + 提前退出+异常真运行）
    codegen/test_codegen_artifact_key.cpp    # bug-94（依赖 #include 名 + thunk 前缀用身份键：artifactKey 传递链路）
    codegen/test_codegen_enum.cpp            # feature-20 批 3/4（enum CodeGen 面：壳/成员函数/match/形态 2/保留名负例）
    codegen/test_codegen_enum_import.cpp     # feature-20 批 6（B2）：跨模块（导入）enum 的装填（多文件 CodeGen 驱动：值语义/限定壳名/本地不变）

    # ---- 诊断引擎 ----
    diag/test_diag.cpp

    # ---- 模块管理 ----
    module/test_module.cpp
    module/test_module_scan.cpp
    module/test_const_expand.cpp     # feature-21 Phase 2（§4.1：#const 展开 / 单文件驱动）
    module/test_macro_import.cpp     # feature-21 Phase 2（§4.2：宏生成 import / epoch 收敛）

    # ---- 编译期求值（feature-21 Phase 3：Value / DeclIndex / EvalPass）----
    compile_time/test_eval.cpp

    # ---- 端到端集成测试 ----
    integration/test_examples.cpp
)

find_package(Threads REQUIRED)

# ccache：测试构建同样吃缓存（2026-11-08）
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS ">>> ccache: ${CCACHE_PROGRAM}")
endif()

# ============================================================
# 编译器 / 运行时核心：OBJECT 库只编一次，供所有测试 exe 复用
# ============================================================
add_library(aura_test_core OBJECT ${AURA_LIB_SOURCES} ${AURA_RT_SOURCES})
target_include_directories(aura_test_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(aura_test_core PRIVATE
    AURA_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# ============================================================
# 测试按类别拆分；测试源用 OBJECT 库只编一次。
#   aura_tests_<cat>  : 单类别 exe（只编/链接该类）
#   aura_tests        : 兼容总入口（链接全部 OBJECT，不重复编译测试源）
# ============================================================
set(AURA_TEST_CATEGORIES rt lexer parser ast sema codegen diag module compile_time integration)
set(AURA_TEST_OBJECTS "")

function(aura_add_test_exe name)
    add_executable(${name} framework/test_main.cpp
        $<TARGET_OBJECTS:aura_test_core> ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_definitions(${name} PRIVATE
        AURA_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    target_link_libraries(${name} PRIVATE Threads::Threads)
endfunction()

enable_testing()

foreach(cat IN LISTS AURA_TEST_CATEGORIES)
    set(cat_sources ${AURA_TEST_SOURCES})
    list(FILTER cat_sources INCLUDE REGEX "^${cat}/")
    if(NOT cat_sources)
        continue()
    endif()
    add_library(aura_tests_${cat}_obj OBJECT ${cat_sources})
    target_include_directories(aura_tests_${cat}_obj PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_definitions(aura_tests_${cat}_obj PRIVATE
        AURA_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    aura_add_test_exe(aura_tests_${cat} $<TARGET_OBJECTS:aura_tests_${cat}_obj>)
    add_test(NAME aura_tests_${cat} COMMAND aura_tests_${cat})
    list(APPEND AURA_TEST_OBJECTS $<TARGET_OBJECTS:aura_tests_${cat}_obj>)
endforeach()

# 兼容既有命令：.	estuildura_tests.exe
aura_add_test_exe(aura_tests ${AURA_TEST_OBJECTS})
